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
#include "hw.h"
#include "ufs.h"
#include "mmu.h"
#include "store.h"
#include "catalog.h"
#include "recovery.h"
#include "post.h"
#include "fastboot.h"
#include "ramboot.h"
#include "sbfmt.h"
#include "crash.h"
#include "wdt.h"
#include "dtmerge.h"

void star_intro_splash(void);   /* anim.c */
uint32_t star_crc32(uintptr_t p, uint64_t n);   /* store.c */

#define LOG_BASE 0x91fff000UL
#define RAM_BASE 0x92000000UL
#define RAM_END  0xaf800000UL
/* Second DRAM bank (TWRP /proc/iomem 880000000-8ffffffff, 2 GiB, no
 * reserved-memory node): XNU may own it (tools/mkadt_starlte.py --bank2). */
#define BANK2_BASE 0x880000000UL
#define BANK2_END  0x900000000UL
#define DRAM_LOW 0x80000000UL
#define PAGE_SIZE 4096UL
#define BOOT_LINE_LENGTH 608
/* xnu-11417 / xnu-12377 boot_args: CommandLine[1024] (pexpert/arm64/boot.h); 608 is the 8796 layout. A
 * version-2 payload (same header, tools/payload-v2.py) says its kernel takes the 1024 layout: the boot-args
 * the loader builds may then grow to 1023 bytes (the payload's own line still holds 607). */
#define XNU_LINE_MAX 1024
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
/* The same fields with the 1024-byte CommandLine (kBootArgsRevision2, xnu-11417 and later). */
struct boot_args_1024 {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[XNU_LINE_MAX];
    uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct boot_args_1024, CommandLine) == 108, "boot_args 1024 layout");
_Static_assert(__builtin_offsetof(struct boot_args_1024, bootFlags) == 1136, "boot_args 1024 bootFlags");
_Static_assert(__builtin_offsetof(struct boot_args_1024, memSizeActual) == 1144, "boot_args 1024 memSizeActual");
_Static_assert(sizeof(struct boot_args_1024) == 1152, "boot_args 1024 size");

/* 8 bytes at a time when both ends are aligned (the OS areas are non-cacheable: mmu.c). */
static void zero_bytes(uintptr_t dst, uint64_t size)
{
    uint64_t i = 0;
    if (!(dst & 7))
        for (; i + 8 <= size; i += 8)
            *(volatile uint64_t *)(dst + i) = 0;
    for (; i < size; ++i)
        *(volatile uint8_t *)(dst + i) = 0;
}

static void copy_bytes(uintptr_t dst, uintptr_t src, uint64_t size)
{
    uint64_t i = 0;
    if (!((dst | src) & 7))
        for (; i + 8 <= size; i += 8)
            *(volatile uint64_t *)(dst + i) = *(volatile const uint64_t *)(src + i);
    for (; i < size; ++i)
        *(volatile uint8_t *)(dst + i) = *(volatile const uint8_t *)(src + i);
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

/* An OS could not be started: why, on screen and in the log. The menu comes back after. */
static const char *boot_why;

static uintptr_t fail(volatile uint64_t *log, int64_t step, const char *why)
{
    log[10] = (uint64_t)step;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    boot_why = why;
    star_fb_text(3, "LOADER FAIL:");
    star_fb_text(4, why);
    return 0;
}

/* No menu to go back to: give time for a photo, then request Download mode. */
static uintptr_t fatal(volatile uint64_t *log, int64_t step, const char *why)
{
    uint64_t freq;
    star_fb_mute(0);                /* menu v7 muted the status lines; no menu comes back now */
    if (why)
        (void)fail(log, step, why);
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
 *   "SBSPLASH" u32 w, h + RGB565: a full-screen picture (splash.mjs);
 *   "SBMENU01" u32 n, n x { u32 id, x, y, w, h, runs; runs x { u32 length,
 *     u32 0xAARRGGBB } }: opaque sprites for the v5 boot menu (menu.mjs),
 *     id 0 = drawn at once, others drawn by menu.c through star_sprite().
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

/* Draw every SBMENU01 sprite with this id (opaque copy). Returns how many
 * were drawn, or -1 if the blob is malformed. */
static int
sprite_walk_at(uint64_t start, uint64_t end, uint32_t want, int ox, int oy, int at);

static int
sprite_walk(uint64_t start, uint64_t end, uint32_t want)
{
    return sprite_walk_at(start, end, want, 0, 0, 0);
}

/* As sprite_walk; with at != 0 the sprite goes to (ox, oy) instead. */
static int
sprite_walk_to(uint64_t start, uint64_t end, uint32_t want, int ox, int oy, int at, volatile uint32_t *fb,
               unsigned fade);

static int
sprite_walk_at(uint64_t start, uint64_t end, uint32_t want, int ox, int oy, int at)
{
    return sprite_walk_to(start, end, want, ox, oy, at, (volatile uint32_t *)STAR_FB_BASE, 256);
}

/* Scale a colour by fade/256 (the sprites are opaque on black, so this fades them in). */
static uint32_t
fade_px(uint32_t c, unsigned fade)
{
    return 0xff000000U | ((((c >> 16) & 255U) * fade >> 8) << 16) | ((((c >> 8) & 255U) * fade >> 8) << 8) |
           ((c & 255U) * fade >> 8);
}

/* As sprite_walk_at, into fb (a panel-sized buffer: the panel itself or anim.c's) at fade/256. */
static int
sprite_walk_to(uint64_t start, uint64_t end, uint32_t want, int ox, int oy, int at, volatile uint32_t *fb,
               unsigned fade)
{
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)start;
    uint32_t n = h[2];
    const volatile uint32_t *q = h + 3;
    int drawn = 0;
    for (uint32_t r = 0; r < n && r < 64; r++) {
        if ((uintptr_t)(q + 6) > end)
            return -1;
        uint32_t id = q[0], x = q[1], y = q[2], w = q[3], ht = q[4], runs = q[5];
        q += 6;
        if (!w || (uintptr_t)(q + 2 * (uint64_t)runs) > end || x + w > STAR_FB_WIDTH ||
            y + ht > STAR_FB_HEIGHT)
            return -1;
        if (id == want && at) {
            if (ox < 0 || oy < 0 || (uint32_t)ox + w > STAR_FB_WIDTH || (uint32_t)oy + ht > STAR_FB_HEIGHT)
                return -1;
            x = (uint32_t)ox;
            y = (uint32_t)oy;
        }
        if (id == want) {
            uint64_t left = (uint64_t)w * ht;
            uint32_t px = 0;
            volatile uint32_t *row = fb + (uint64_t)y * STAR_FB_WIDTH + x;
            for (uint32_t k = 0; k < runs; k++) {
                uint32_t length = q[2 * k], c = q[2 * k + 1];
                if (length > left)
                    return -1;
                left -= length;
                if (fade < 256)
                    c = fade_px(c, fade);
                for (; length; --length) {
                    row[px] = c;
                    if (++px == w) {
                        px = 0;
                        row += STAR_FB_WIDTH;
                    }
                }
            }
            drawn++;
        }
        q += 2 * (uint64_t)runs;
    }
    return drawn;
}

static uint64_t menu_start, menu_end;

int
star_menu_assets(void)
{
    return menu_end > menu_start;
}

/* anim.c: sprite id into dst (0 = the panel), at (x, y) when at, faded to fade/256. */
int
star_sprite_to(unsigned id, uint32_t *dst, int x, int y, int at, unsigned fade)
{
    if (!star_menu_assets())
        return 0;
    return sprite_walk_to(menu_start, menu_end, id, x, y, at,
                          dst ? (volatile uint32_t *)dst : (volatile uint32_t *)STAR_FB_BASE, fade);
}

int
star_sprite_at(unsigned id, int x, int y)
{
    if (!star_menu_assets())
        return 0;
    return sprite_walk_at(menu_start, menu_end, id, x, y, 1);
}

/* The EngineerMode glyph atlas ("SBFONT01", tools/brand/em-font.py) follows
 * the SBMENU01 sprites in the same ramdisk blob. */
int
star_font_blob(uint64_t *start, uint64_t *end)
{
    if (!star_menu_assets())
        return 0;
    const volatile uint32_t *hd = (const volatile uint32_t *)(uintptr_t)menu_start;
    const volatile uint32_t *q = hd + 3;
    for (uint32_t r = 0; r < hd[2]; r++) {
        if ((uintptr_t)(q + 6) > menu_end)
            return 0;
        q += 6 + 2 * (uint64_t)q[5];
    }
    if ((uintptr_t)(q + 3) > menu_end || q[0] != 0x4f464253U || q[1] != 0x3130544eU)   /* "SBFONT01" */
        return 0;
    *start = (uintptr_t)q;
    *end = menu_end;
    return 1;
}

int
star_sprite_rect(unsigned id, unsigned *x, unsigned *y, unsigned *w, unsigned *h)
{
    if (!star_menu_assets())
        return 0;
    const volatile uint32_t *hd = (const volatile uint32_t *)(uintptr_t)menu_start;
    const volatile uint32_t *q = hd + 3;
    for (uint32_t r = 0; r < hd[2] && r < 64; r++) {
        if ((uintptr_t)(q + 6) > menu_end)
            return 0;
        if (q[0] == id) {
            *x = q[1];
            *y = q[2];
            *w = q[3];
            *h = q[4];
            return 1;
        }
        q += 6 + 2 * (uint64_t)q[5];
    }
    return 0;
}

int
star_sprite(unsigned id)
{
    if (!star_menu_assets())
        return 0;
    return sprite_walk(menu_start, menu_end, id);
}

static int
star_splash_draw(void)
{
    uint64_t start = splash_start, end = splash_end;
    if (end <= start + 16)
        return 0;
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)start;
    volatile uint32_t *fb = (volatile uint32_t *)STAR_FB_BASE;
    if (h[0] == 0x454d4253U && h[1] == 0x3130554eU) {           /* "SBMENU01" */
        if (sprite_walk(start, end, 0xffffffffU) < 0)
            return 0;
        menu_start = start;
        menu_end = end;
        /* S-Boot's splash -> the menu's base, animated (anim.c; plain without the MMU). Menu v7
         * leaves S-Boot's splash on the panel: its intro starts from it (menu.c falls back to this
         * intro if v7 cannot run). */
        if (!menu7_ready())
            star_intro_splash();
        star_fb_row_base(36);
        return 1;
    }
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
 * Where OSes are put before they start (all Normal non-cacheable, mmu.c, so the store's DMA needs
 * no cache maintenance):
 *   STAGE  0x8c000000..0x91000000  an XNU payload from the store (it must end below RAM_BASE), or
 *                                  a UEFI image (below 0x90000000, where its FD goes)
 *   RAM_BASE + text_offset         Linux, Android and bare arm64 Images (up to OS_RD)
 *   OS_RD  0xa8000000              an initrd, Android's ramdisk
 *   EDIT_FDT 0xad000000            an FDT being edited (Android: S-Boot's; Linux: the store's)
 *   LINUX_DTB 0xae000000           Linux's DTB, up to UFS_DMA
 */
#define STAGE      0x8c000000UL
#define STAGE_END  0x91000000UL
#define UEFI_FD    0x90000000UL
#define UEFI_FD_END 0x90700000UL
#define LINUX_BASE RAM_BASE
#define OS_RD      0xa8000000UL
#define EDIT_FDT   0xad000000UL
#define LINUX_DTB  0xae000000UL
#define HEAD_BUF   (UFS_BUF + (768U << 10))   /* an Image's first block */
#define TEXT_BUF   (UFS_BUF + (832U << 10))   /* a command line file */

/*
 * BOOT itself: the SWEETXNU payload appended to this Image (or S-Boot's initrd, as a fallback),
 * then bundles (tools/linux-bundle.py), each a 4 KiB page with u64 magic + u64 size, the data,
 * padding to 4 KiB: "SBUEFI!!" the Windows UEFI (and "SBLINUX!", the retired Linux kiosk
 * kernel, in older images). Without a valid payload, bundles are found by scanning pages: no OS
 * depends on iOS any more.
 */
#define BUNDLE_LINUX 0x2158554e494c4253ULL   /* "SBLINUX!" */
#define BUNDLE_UEFI  0x2121494645554253ULL   /* "SBUEFI!!" */
#define BUNDLE_RESCUE 0x4555435345524253ULL  /* "SBRESCUE": Starbite Rescue (tools/rescue/bundle-rescue.py) */

static const struct payload *boot_p;          /* BOOT's payload when it is valid */
static uintptr_t boot_rd_start, boot_rd_end;  /* what is appended to this Image */

/* Why payload p (length bytes) cannot boot, or 0. */
static const char *payload_bad(const struct payload *p, uint64_t length)
{
    if (length < sizeof(*p) || !magic_ok(p->magic) || (p->version != 1 && p->version != 2) ||
        !p->nsegs || p->nsegs > 32 ||
        __builtin_offsetof(struct payload, segs) + p->nsegs * sizeof(struct payload_seg) > length ||
        p->adt_off > length || p->adt_size > length - p->adt_off)
        return "BAD SWEETXNU PAYLOAD";
    /* The payload itself sits below RAM_BASE, outside both windows. */
    uint64_t window_end = p->phys_base >= BANK2_BASE ? BANK2_END : RAM_END;
    int in_low = p->phys_base >= RAM_BASE && p->phys_base < RAM_END;
    int in_bank2 = p->phys_base >= BANK2_BASE && p->phys_base < BANK2_END;
    if (!(in_low || in_bank2) ||
        !p->mem_size || p->mem_size > window_end - p->phys_base ||
        p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
        return "PAYLOAD RAM OUTSIDE WINDOW";
    return 0;
}

static uint64_t payload_end(const struct payload *p)
{
    uint64_t end = __builtin_offsetof(struct payload, segs) + p->nsegs * sizeof(struct payload_seg);
    if (p->adt_off + p->adt_size > end)
        end = p->adt_off + p->adt_size;
    for (uint32_t i = 0; i < p->nsegs; ++i)
        if (p->segs[i].file_off + p->segs[i].file_size > end)
            end = p->segs[i].file_off + p->segs[i].file_size;
    return end;
}

static void boot_payload(uintptr_t fdt)
{
    extern char _start[], _image_end[];
    uint64_t image_size = *(volatile uint64_t *)(uintptr_t)(_start + 16);
    uint64_t rs = (uintptr_t)_image_end, re = (uintptr_t)_start + image_size;
    if (re <= rs || !magic_ok((const char *)rs)) {
        uint64_t s, e;
        if (fdt && !(fdt & 7) && fdt_chosen_u64(fdt, "linux,initrd-start", &s) &&
            fdt_chosen_u64(fdt, "linux,initrd-end", &e) && e > s && s >= DRAM_LOW && e <= RAM_BASE &&
            magic_ok((const char *)(uintptr_t)s)) {
            rs = s;
            re = e;
        }
    }
    if (re <= rs || rs < DRAM_LOW || re > RAM_BASE)
        return;
    boot_rd_start = rs;
    boot_rd_end = re;
    if (!payload_bad((const struct payload *)(uintptr_t)rs, re - rs))
        boot_p = (const struct payload *)(uintptr_t)rs;
}

static uintptr_t find_bundle(uint64_t magic, uint64_t *size)
{
    if (boot_rd_end <= boot_rd_start)
        return 0;
    uintptr_t b = boot_p ? align_page(boot_rd_start + payload_end(boot_p)) : boot_rd_start;
    while (b + PAGE_SIZE < boot_rd_end) {
        uint64_t m = *(volatile uint64_t *)b, s = *(volatile uint64_t *)(b + 8);
        if ((m == BUNDLE_LINUX || m == BUNDLE_UEFI || m == BUNDLE_RESCUE) && s <= boot_rd_end - b - PAGE_SIZE) {
            if (m == magic) {
                *size = s;
                return b + PAGE_SIZE;
            }
            b = align_page(b + PAGE_SIZE + s);
        } else if (boot_p) {
            return 0;                       /* bundles follow each other after the payload */
        } else {
            b += PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t
star_bundle(const struct star_info *in, int linux_not_uefi, uint64_t *size)
{
    (void)in;
    *size = 0;
    return find_bundle(linux_not_uefi ? BUNDLE_LINUX : BUNDLE_UEFI, size);
}

/* ---- the files of a catalog entry ----------------------------------------- */

#define SRC_NONE   0
#define SRC_STORE  1
#define SRC_BOOT   2   /* BOOT's payload */
#define SRC_BUNDLE 3   /* BOOT's UEFI bundle */

struct src {
    int kind;
    struct store_ent e;
    uintptr_t addr;
    uint64_t size;
};

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* The first alternative of a file field that exists. */
static int resolve(const char *field, struct src *s)
{
    char alt[64];
    s->kind = SRC_NONE;
    for (unsigned k = 0; cat_alt(field, k, alt); ++k) {
        if (str_eq(alt, "@boot")) {
            if (!boot_p)
                continue;
            s->kind = SRC_BOOT;
            s->addr = boot_rd_start;
            s->size = boot_rd_end - boot_rd_start;
            return 1;
        }
        if (str_eq(alt, "@bundle")) {
            uint64_t size;
            uintptr_t at = find_bundle(BUNDLE_UEFI, &size);
            if (!at)
                continue;
            s->kind = SRC_BUNDLE;
            s->addr = at;
            s->size = size;
            return 1;
        }
        if (store_find(alt, &s->e)) {
            s->kind = SRC_STORE;
            s->size = s->e.size;
            return 1;
        }
    }
    return 0;
}

int star_entry_ready(unsigned i, const char **why)
{
    const struct cat_ent *e = cat_get(i);
    struct src s;
    const char *w = 0;
    if (!e)
        w = "NO SUCH ENTRY";
    else if (e->method < M_XNU || e->method > M_BARE)
        w = "UNKNOWN BOOT METHOD";
    else if (!resolve(e->file[0], &s))
        w = e->method == M_XNU ? "NO XNU PAYLOAD" : e->method == M_UEFI ? "NO UEFI IMAGE" : "NO KERNEL";
    else if (e->method == M_LINUX && !resolve(e->file[1], &s))
        w = "NO DTB";
    else if (e->method == M_ANDROID && !resolve(e->file[1], &s))
        w = "NO RAMDISK";
    else if (e->method == M_ANDROID && e->file[3][0] && !resolve(e->file[3], &s))
        w = "NO ANDROID DTB";
    if (why)
        *why = w;
    return !w;
}

/* " extra" onto a NUL-terminated line of cap bytes. */
static void line_append(char *c, unsigned cap, const char *extra)
{
    if (!extra || !extra[0])
        return;
    unsigned n = 0;
    while (n + 1 < cap && c[n])
        ++n;
    if (n && n + 1 < cap)
        c[n++] = ' ';
    for (unsigned k = 0; extra[k] && n + 1 < cap; ++k)
        c[n++] = extra[k];
    c[n] = 0;
}

/* Menu v7's launch card follows the real stages (menu.c forwards these while v7 is the menu):
 * stage k of n, what is happening and a detail line ("linux.image · 31.4 MB", "0x9A3F1C07"). */
struct pline { char b[64]; unsigned n; };

static void pl_s(struct pline *p, const char *s)
{
    while (s && *s && p->n + 1 < sizeof(p->b))
        p->b[p->n++] = *s++;
    p->b[p->n] = 0;
}

static void pl_u(struct pline *p, uint64_t v)
{
    char d[20];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < sizeof(d));
    while (k && p->n + 1 < sizeof(p->b))
        p->b[p->n++] = d[--k];
    p->b[p->n] = 0;
}

static void pl_hex(struct pline *p, uint64_t v, unsigned digits)
{
    pl_s(p, "0x");
    while (digits-- && p->n + 1 < sizeof(p->b)) {
        unsigned d = (unsigned)(v >> (4 * digits)) & 15U;
        p->b[p->n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    p->b[p->n] = 0;
}

/* "31.4 MB", or "212 KB" below 1 MiB */
static void pl_size(struct pline *p, uint64_t bytes)
{
    if (bytes < (1U << 20)) {
        pl_u(p, (bytes + 512) >> 10);
        pl_s(p, " KB");
        return;
    }
    uint64_t t = (bytes * 10 + (1U << 19)) >> 20;
    pl_u(p, t / 10);
    pl_s(p, ".");
    pl_u(p, t % 10);
    pl_s(p, " MB");
}

static void stage_text(unsigned k, unsigned n, const char *title, const char *a, uint64_t size)
{
    struct pline p;
    p.n = 0;
    p.b[0] = 0;
    pl_s(&p, a);
    if (size) {
        pl_s(&p, a && a[0] ? " \xc2\xb7 " : "");
        pl_size(&p, size);
    }
    star_menu_progress(k, n, title, p.b);
}

static void stage_file(unsigned k, unsigned n, const char *title, const struct store_ent *e)
{
    stage_text(k, n, title, e->name, e->size);
}

static void stage_crc(unsigned k, unsigned n, const char *title, uint32_t crc)
{
    struct pline p;
    p.n = 0;
    pl_hex(&p, crc, 8);
    star_menu_progress(k, n, title, p.b);
}

/* the last stage, then v7's handoff screen ("EL1 · 0x92080000") */
static void stage_entry(unsigned k, unsigned n, const char *title, uintptr_t entry)
{
    struct pline p;
    p.n = 0;
    pl_s(&p, "EL1 \xc2\xb7 ");
    pl_hex(&p, entry, 8);
    star_menu_progress(k, n, title, p.b);
    star_menu_handoff(p.b);
}

/* An arm64 Image from the store at RAM_BASE + text_offset, ending below limit: its address, or
 * 0 after fail(). *span = its size in memory. */
static uintptr_t image_load(volatile uint64_t *log, const struct store_ent *e, uintptr_t limit,
    int64_t step, uint64_t *span)
{
    if (store_head(e, HEAD_BUF) || *(volatile uint32_t *)(HEAD_BUF + 56) != 0x644d5241U)
        return fail(log, step, "NOT AN ARM64 IMAGE");
    uint64_t text_offset = *(volatile uint64_t *)(HEAD_BUF + 8);
    uint64_t image_size = *(volatile uint64_t *)(HEAD_BUF + 16);
    if (image_size < e->size)
        image_size = e->size;
    uintptr_t dst = LINUX_BASE + text_offset;
    if (text_offset >= (2UL << 20) || image_size > limit - dst)
        return fail(log, step - 1, "KERNEL TOO LARGE");
    star_fb_hex(2, "KERNEL AT", dst);
    int r = store_load(e, dst, limit - dst);
    if (r)
        return fail(log, step - 2, r == -2 ? "KERNEL CRC MISMATCH" : "KERNEL READ FAILED");
    *span = image_size;
    return dst;
}

/*
 * ---- The launch: log level, watchdog, crash log (docs/STARBITE-HEALTH.md) ---------------------
 * boot_entry works on a copy of the catalog entry whose cmdline has the global log level's
 * fragment (catalog extension) and the watchdog's kernel arguments appended; XNU gets its fragment
 * through launch.xfrag, where "!token" removes a boot-arg the payload already has. Once an entry
 * point exists, launch_go writes the launch record (crash.c) and arms the watchdog (wdt.c), the
 * last two things before the OS starts.
 */
static struct {
    int valid, inform3, retry;
    char key[24], name[32];
    unsigned method, wdt, lvl, flags;
    char xfrag[CAT_LOG_MAX + 32];
} launch;
static struct cat_ent launch_ent;

static void lscopy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    for (; s && s[k] && k + 1 < cap; ++k)
        d[k] = s[k];
    d[k] = 0;
}

static void launch_clear(void)
{
    launch.valid = launch.inform3 = 0;
    launch.wdt = launch.flags = 0;
    launch.xfrag[0] = 0;
}

static void launch_set(const char *key, const char *name, unsigned method, unsigned flags)
{
    struct star_settings s;
    launch_clear();
    if (!settings_load(&s))
        sbset_defaults(&s);
    lscopy(launch.key, sizeof(launch.key), key);
    lscopy(launch.name, sizeof(launch.name), name);
    launch.method = method;
    launch.lvl = sbset_log(&s);
    launch.flags = flags;
    launch.valid = 1;
}

/* " name=N" onto a command line of cap bytes */
static void arg_num(char *line, unsigned cap, const char *name, unsigned v)
{
    char t[48];
    unsigned n = 0;
    for (; name[n] && n < 32; ++n)
        t[n] = name[n];
    char d[10];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < sizeof(d));
    while (k)
        t[n++] = d[--k];
    t[n] = 0;
    (void)sb_line_edit(line, cap, t, 0);
}

/* The watchdog's arguments did not make it into the command line (sb_line_edit drops tokens that
 * do not fit): it is not armed for this start. Log slot 19 bit 16 records it. Returns 0. */
static void pmsg_put(const char *s);
static unsigned wdt_dropped(volatile uint64_t *log, const char *key, const char *what, unsigned wdt)
{
    char b[160];
    unsigned n = 0;
    const char *parts[5] = { "STARBITE: watchdog NOT armed for ", key, ": ", what, " did not fit the command line (" };
    for (unsigned p = 0; p < 5; ++p)
        for (const char *s = parts[p]; *s && n + 1 < sizeof(b); ++s)
            b[n++] = *s;
    char d[4];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + wdt % 10U);
        wdt /= 10U;
    } while (wdt && k < sizeof(d));
    while (k && n + 1 < sizeof(b))
        b[n++] = d[--k];
    const char *tail = " s asked)\n";
    for (; *tail && n + 1 < sizeof(b); ++tail)
        b[n++] = *tail;
    b[n] = 0;
    pmsg_put(b);
    log[19] = (log[19] & ~(0xffffffffULL << 32)) | 1ULL << 16;
    return 0;
}

/* The entry as it will be started (launch_ent) and the launch's state. */
static const struct cat_ent *launch_prepare(const struct cat_ent *e, volatile uint64_t *log)
{
    struct star_settings s;
    if (!settings_load(&s))
        sbset_defaults(&s);
    const uint8_t *src = (const uint8_t *)e;
    uint8_t *dst = (uint8_t *)&launch_ent;
    for (unsigned i = 0; i < sizeof(launch_ent); ++i)
        dst[i] = src[i];
    launch_clear();
    unsigned lvl = sbset_log(&s);
    const char *frag = e->log[lvl];
    /* the watchdog: only where someone will feed it */
    unsigned wdt = 0, feed = e->wdt_feed, dropped = 0;
    int fits = (feed == CW_KERNEL || feed == CW_USERSPACE) ? e->method == M_LINUX
             : feed == CW_XNU ? e->method == M_XNU : feed == CW_OWN;
    if (s.wdt_mode != SET_WDT_OFF && e->wdt_s && fits)
        wdt = s.wdt_mode == SET_WDT_FIXED && s.wdt_s ? s.wdt_s : e->wdt_s;
    if (wdt) {
        unsigned ms;
        (void)sb_wdt_count(wdt, &ms);
        wdt = (ms + 500U) / 1000U;
    }
    if (e->method == M_XNU) {
        lscopy(launch.xfrag, sizeof(launch.xfrag), frag);
        if (wdt)
            arg_num(launch.xfrag, sizeof(launch.xfrag), "sbwdt=", wdt);
    } else if (e->method == M_LINUX || e->method == M_ANDROID) {
        (void)sb_line_edit(launch_ent.cmdline, sizeof(launch_ent.cmdline), frag, 0);
        if (wdt && e->method == M_LINUX) {
            (void)sb_line_edit(launch_ent.cmdline, sizeof(launch_ent.cmdline), "s3c2410_wdt.tmr_atboot=1", 0);
            arg_num(launch_ent.cmdline, sizeof(launch_ent.cmdline), "s3c2410_wdt.tmr_margin=", wdt);
            if (feed == CW_USERSPACE)
                arg_num(launch_ent.cmdline, sizeof(launch_ent.cmdline), "sb.wdt=", wdt);
            /* sb_line_edit drops what does not fit: without both arguments mainline's driver stops
             * the watchdog at probe (tmr_atboot=0 from the DTB) or runs it with its own margin, so
             * nothing would feed ours. Then it is not armed at all. */
            if (!sb_wdt_args_ok(launch_ent.cmdline, sizeof(launch_ent.cmdline), SB_WDT_LINUX, wdt))
                wdt = wdt_dropped(log, e->key, "s3c2410_wdt.tmr_atboot=1 tmr_margin", wdt), dropped = 1;
        }
    }
    lscopy(launch.key, sizeof(launch.key), e->key);
    lscopy(launch.name, sizeof(launch.name), e->name);
    launch.method = e->method;
    launch.lvl = lvl;
    launch.wdt = wdt;
    launch.inform3 = (e->cflags & CE_LINUX_INFORM3) && e->method == M_LINUX && s.crash_mode != SET_CRASH_OFF;
    launch.flags = (launch.inform3 ? CLF_LINUX_INFORM3 : 0U) | (launch.retry ? CLF_RETRY : 0U);
    launch.valid = !(e->cflags & CE_NO_LAUNCH);
    log[19] = (uint64_t)wdt << 32 | (uint64_t)dropped << 16 | (uint64_t)lvl << 8 | (uint64_t)(unsigned)launch.inform3;
    return &launch_ent;
}

/* The last things before the OS: the launch record, then the watchdog. */
static void pmsg_put(const char *s);
static void launch_go(volatile uint64_t *log)
{
    if (!launch.valid)
        return;
    crash_launch(launch.key, launch.name, launch.method, launch.wdt, launch.wdt != 0, launch.lvl, launch.flags);
    /* docs/MAINTENANCE.md 4.2 R7: the start an "SBNEXT2R" mailbox asked for leaves its return key
     * in the mailbox, so the test OS's reset comes back there (to SBMAINT) instead of RECOVERY */
    const char *rk = rec_return_key(launch.key);
    if (rk) {
        rec_mailbox_write(rk);
        pmsg_put("STARBITE: boot-next return key armed: ");
        pmsg_put(rk);
        pmsg_put("\n");
    }
    if (launch.wdt) {
        unsigned ms = wdt_arm(launch.wdt);
        log[19] |= (uint64_t)ms << 40;
        pmsg_put(ms ? "STARBITE: watchdog armed (cluster 1, 0x10060000)\n" : "STARBITE: watchdog NOT armed\n");
    }
    launch.valid = 0;
}

/*
 * Linux: Image + DTB (+ initrd) from the store; the entry's cmdline is appended to the DTB's
 * bootargs. x0 = DTB. The display trigger stays free-running (0x1281, as uniLoader leaves it):
 * mainline's framebuffer driver depends on it. The microSD rails are switched on for it.
 */
static uintptr_t
boot_linux(volatile uint64_t *log, const struct cat_ent *e)
{
    struct src img, dtb, rd;
    star_fb_text(1, "LOADING LINUX");
    if (!resolve(e->file[0], &img) || img.kind != SRC_STORE)
        return fail(log, -41, "NO LINUX KERNEL IN THE STORE");
    if (!resolve(e->file[1], &dtb) || dtb.kind != SRC_STORE)
        return fail(log, -41, "NO LINUX DTB IN THE STORE");
    int have_rd = e->file[2][0] != 0;
    if (have_rd && (!resolve(e->file[2], &rd) || rd.kind != SRC_STORE))
        return fail(log, -47, "NO INITRD IN THE STORE");
    unsigned ns = have_rd ? 5 : 4, k = 0;
    stage_file(k++, ns, "Loading device tree", &dtb.e);
    int r = store_load(&dtb.e, e->cmdline[0] ? EDIT_FDT : LINUX_DTB, 2UL << 20);
    if (r)
        return fail(log, -43, r == -3 ? "LINUX DTB TOO LARGE" : "LINUX DTB READ OR CRC FAILED");
    uint64_t dtb_size = dtb.e.size;
    if (e->cmdline[0]) {
        dtb_size = fdt_copy_append_bootargs(EDIT_FDT, LINUX_DTB, (uint32_t)(UFS_DMA - LINUX_DTB), e->cmdline);
        if (!dtb_size)
            return fail(log, -48, "LINUX DTB HAS NO BOOTARGS");
    }
    /* Wall clock for Linux (no RTC driver there): the DTB carries a u64
     * placeholder /chosen/starbite,rtc-epoch; tools/linux/init reads it. */
    uint64_t now;
    if (em_rtc_epoch(&now))
        (void)fdt_chosen_set_u64(LINUX_DTB, "starbite,rtc-epoch", now);
    /* Crash detection: the port's DTB sets INFORM3 = recovery at boot (s9p,set-registers), so a
     * panic or a watchdog reset would land in RECOVERY. The launch's copy says normal instead, and
     * the reset comes back here (catalog flag CE_LINUX_INFORM3). */
    if (launch.inform3)
        log[20] = (uint64_t)fdt_patch_pairs((uint8_t *)LINUX_DTB, "s9p,set-registers", 0x1406080cU,
                                            INFORM_RECOVERY, INFORM_NORMAL);
    if (have_rd) {
        stage_file(k++, ns, "Loading initramfs", &rd.e);
        r = store_load(&rd.e, OS_RD, EDIT_FDT - OS_RD);
        if (r)
            return fail(log, -49, r == -3 ? "INITRD TOO LARGE" : "INITRD READ OR CRC FAILED");
        if (!fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-start", OS_RD) ||
            !fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-end", OS_RD + rd.e.size))
            return fail(log, -49, "LINUX DTB HAS NO INITRD PROPERTIES");
    }
    uint8_t l31 = 0, l2 = 0;
    int rails = hw_sd_rails(&l31, &l2);
    star_fb_text(3, rails > 0 ? "MICROSD POWER ON" : rails == 0 ? "MICROSD POWER REFUSED" : "MICROSD POWER: NO ACPM");
    log[18] = (uint64_t)l31 << 8 | l2;
    uint64_t span;
    stage_file(k++, ns, "Reading kernel", &img.e);
    uintptr_t dst = image_load(log, &img.e, OS_RD, -44, &span);
    if (!dst)
        return 0;
    stage_crc(k++, ns, "Verifying kernel CRC32", img.e.crc);
    stage_entry(k, ns, "Handing off to kernel", dst);
    clean_bytes(dst, dst + span);
    clean_bytes(LINUX_DTB, LINUX_DTB + dtb_size);
    log[10] = 0x400;
    log[15] = dst;
    log[17] = LINUX_DTB;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/*
 * Android: kernel (arm64 Image, Samsung 4.9) at RAM_BASE + text_offset, ramdisk (first stage
 * init) at OS_RD, and S-Boot's own FDT (the stock DTB it loaded, with its androidboot.*
 * bootargs) copied to EDIT_FDT with the cmdline file and the entry's cmdline appended and
 * /chosen linux,initrd-* pointing at the ramdisk. With a 4th file (the OS's own DTB or DTBH,
 * loaded to LINUX_DTB), that DTB is the base and S-Boot's run-time state is merged into it
 * (dtmerge.c); without one the path is exactly the old one. The display goes back as S-Boot left
 * it: the vendor kernel initialises DECON itself.
 */
static uintptr_t
boot_android(volatile uint64_t *log, uintptr_t fdt, const struct cat_ent *e)
{
    struct src img, rd, cl;
    static char cmdline[1024];
    star_fb_text(1, "LOADING ANDROID");
    if (!resolve(e->file[0], &img) || img.kind != SRC_STORE ||
        !resolve(e->file[1], &rd) || rd.kind != SRC_STORE)
        return fail(log, -51, "NO ANDROID IN THE OS STORE");
    cmdline[0] = 0;
    if (e->file[2][0] && resolve(e->file[2], &cl) && cl.kind == SRC_STORE && cl.e.size < sizeof(cmdline) - 1) {
        if (store_load(&cl.e, TEXT_BUF, sizeof(cmdline) - 1))
            return fail(log, -52, "ANDROID CMDLINE READ OR CRC FAILED");
        for (uint32_t i = 0; i < cl.e.size; ++i) {
            char c = *(volatile char *)(TEXT_BUF + i);
            cmdline[i] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        cmdline[cl.e.size] = 0;
    }
    line_append(cmdline, sizeof(cmdline), e->cmdline);
    stage_file(0, 4, "Loading ramdisk", &rd.e);
    int r = store_load(&rd.e, OS_RD, EDIT_FDT - OS_RD);
    if (r)
        return fail(log, -54, r == -3 ? "ANDROID RAMDISK TOO LARGE" : "ANDROID RAMDISK READ OR CRC FAILED");
    uint64_t span;
    stage_file(1, 4, "Reading kernel", &img.e);
    uintptr_t dst = image_load(log, &img.e, OS_RD, -55, &span);
    if (!dst)
        return 0;
    uint32_t fsize;
    struct src dt;
    if (e->file[3][0]) {
        /* The entry's own DTB (a plain FDT or Samsung's DTBH container, dtmerge.h) instead of the
         * one S-Boot loaded from BOOT, with what S-Boot set at run time carried over: /chosen
         * (bootargs + this cmdline, initrd, revision, serial), the memory nodes, the root's
         * model_info-system_rev. */
        uint32_t chip, rev, off, dsz;
        unsigned idx, cnt;
        if (!resolve(e->file[3], &dt) || dt.kind != SRC_STORE)
            return fail(log, -56, "ANDROID DTB NOT IN THE STORE");
        stage_file(2, 4, "Loading device tree", &dt.e);
        int rr = store_load(&dt.e, LINUX_DTB, UFS_DMA - LINUX_DTB);
        if (rr)
            return fail(log, -56, rr == -3 ? "ANDROID DTB TOO LARGE" : "ANDROID DTB READ OR CRC FAILED");
        (void)fdt_board_id((const uint8_t *)fdt, &chip, &rev);
        if (!dtbh_pick((const uint8_t *)LINUX_DTB, dt.e.size, chip, rev, &off, &dsz, &idx, &cnt))
            return fail(log, -57, "ANDROID DTB: NO USABLE DEVICE TREE");
        fsize = fdt_merge_sboot((const uint8_t *)(LINUX_DTB + off), (const uint8_t *)fdt, (uint8_t *)EDIT_FDT,
                                (uint32_t)(LINUX_DTB - EDIT_FDT), cmdline);
        if (!fsize)
            return fail(log, -58, "ANDROID DTB MERGE FAILED");
        log[21] = (uint64_t)chip << 48 | (uint64_t)(rev & 0xffffU) << 32 | (uint64_t)idx << 16 | cnt;
    } else {
        stage_text(2, 4, "Loading device tree", "S-Boot FDT", __builtin_bswap32(*(volatile uint32_t *)(fdt + 4)));
        fsize = cmdline[0] ? fdt_copy_append_bootargs(fdt, EDIT_FDT, LINUX_DTB - EDIT_FDT, cmdline) : 0;
        if (!fsize) {
            uint32_t total = __builtin_bswap32(*(volatile uint32_t *)(fdt + 4));
            if (cmdline[0] || total > LINUX_DTB - EDIT_FDT)
                return fail(log, -58, "ANDROID FDT FAILED");
            copy_bytes(EDIT_FDT, fdt, total);
            fsize = total;
        }
    }
    if (!fdt_chosen_set_u64(EDIT_FDT, "linux,initrd-start", OS_RD) ||
        !fdt_chosen_set_u64(EDIT_FDT, "linux,initrd-end", OS_RD + rd.e.size))
        return fail(log, -59, "NO INITRD PROPERTIES IN FDT");
    stage_entry(3, 4, "Handing off to kernel", dst);
    clean_bytes(dst, dst + span);
    clean_bytes(OS_RD, OS_RD + rd.e.size);
    clean_bytes(EDIT_FDT, EDIT_FDT + fsize);
    star_fb_restore((uint32_t)log[77]);
    log[10] = 0x500;
    log[15] = dst;
    log[17] = EDIT_FDT;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/*
 * Bare: any arm64 Image from the store (Starbite's own systems, demos) at RAM_BASE + text_offset,
 * entered like Linux with x0 = S-Boot's FDT and the screen as the loader leaves it (the
 * framebuffer at STAR_FB_BASE, CPU writes reach the panel).
 */
static uintptr_t
boot_bare(volatile uint64_t *log, uintptr_t fdt, const struct cat_ent *e)
{
    struct src s;
    uint64_t span;
    star_fb_text(1, "LOADING");
    if (!resolve(e->file[0], &s) || s.kind != SRC_STORE)
        return fail(log, -60, "NO IMAGE IN THE STORE");
    stage_file(0, 2, "Reading image", &s.e);
    uintptr_t dst = image_load(log, &s.e, OS_RD, -61, &span);
    if (!dst)
        return 0;
    stage_entry(1, 2, "Handing off", dst);
    clean_bytes(dst, dst + span);
    log[10] = 0x600;
    log[15] = dst;
    log[17] = fdt;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/*
 * Windows 11: the exynos9810-woa UEFI (BOOT kernel of its release image:
 * edk2-msm style bootshim + EDK2 FD), from the store ("windows.uefi", put at
 * STAGE) or an "SBUEFI!!" bundle in BOOT. The bootshim is position
 * independent and copies the FD to 0x90000000 itself, so it is entered where
 * it lies, as S-Boot would enter it (x0 = FDT, MMU off).
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
#define RWD1_BASE 0xFED13D80UL   /* drivers/include/Rwd1Constants.h */
#define P3_BASE   0xFED13E80UL   /* SMP1 startup record, 128 bytes */
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

/* for menu7's diagnostics (menu7.c m7_report) */
void star_pmsg(const char *s)
{
    pmsg_put(s);
}

static void star_rwd1_arm(void);

static uintptr_t
boot_uefi(volatile uint64_t *log, uintptr_t fdt, const struct cat_ent *e)
{
    struct src s;
    uintptr_t img;
    uint64_t size;
    if (!resolve(e->file[0], &s))
        return fail(log, -30, "NO WINDOWS UEFI");
    if (s.kind == SRC_STORE) {
        star_fb_text(1, "LOADING UEFI");
        stage_file(0, 3, "Reading firmware", &s.e);
        int r = store_load(&s.e, STAGE, UEFI_FD - STAGE);
        if (r)
            return fail(log, -33, r == -3 ? "UEFI IMAGE TOO LARGE" : r == -2 ? "UEFI IMAGE CRC MISMATCH" : "UEFI IMAGE READ FAILED");
        img = STAGE;
        size = s.e.size;
    } else if (s.kind == SRC_BUNDLE) {
        img = s.addr;
        size = s.size;
    } else {
        return fail(log, -31, "NOT A UEFI IMAGE");
    }
    if (size < 64 || *(volatile uint32_t *)(img + 56) != 0x644d5241U)
        return fail(log, -31, "BAD WINDOWS UEFI");
    /* The FD lands at 0x90000000..0x90700000: the image must not be there. */
    if (img < UEFI_FD_END && img + size > UEFI_FD)
        return fail(log, -32, "UEFI IMAGE OVER ITS FD");
    stage_text(1, 3, "Verifying image", s.kind == SRC_STORE ? "arm64 Image" : "BOOT bundle", size);
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
    star_rwd1_arm();
    stage_entry(2, 3, "Starting firmware", img);
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
 * latch + 'WATL' guard (7F40/44), RAM_STATUS / unattend token (7F60), the
 * witness's usable-page count (7F80) and the Windows handoff marker (7FB0).
 * Then the UFS driver's normal-boot-reason record 'NRM1' (7F18: magic,
 * phase 1=init 2=stop, INFORM2/INFORM3/SYSIP_DAT0 before and after, checksum,
 * 'PMOK') and its Windows witness 'WWR1' (7F70: magic, active procs, max
 * procs, groups, pages lo/hi, range count, flags, tick, checksum). Both are
 * written by Exynos9810Ufs.sys and survive TWRP; a phase-2 NRM1 proves an
 * orderly Windows restart went through the driver's SRB shutdown path.
 */
static void
pmsg_hex32(uint32_t v)
{
    char b[9];
    for (int i = 0; i < 8; ++i)
        b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[8] = 0;
    pmsg_put(b);
}

static void
star_dump_latches(void)
{
    static const uint32_t off[] = { 0x10, 0x14, 0x40, 0x44, 0x60, 0x80, 0xB0 };
    pmsg_put("STARBITE: latches");
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) {
        pmsg_put(" 7f");
        pmsg_hex(*(volatile uint32_t *)(0xFED17F00UL + off[i]));
    }
    /* Firmware<->UFS-driver low-level watchdog handoff record (0xFED13800). */
    pmsg_put(" wdt");
    for (unsigned i = 0; i < 3; ++i) {
        pmsg_put(" ");
        pmsg_hex32(*(volatile uint32_t *)(0xFED13800UL + 4 * i));
    }
    pmsg_put(" nrm");
    for (unsigned i = 0; i < 10; ++i) {
        pmsg_put(" ");
        pmsg_hex32(*(volatile uint32_t *)(0xFED17F18UL + 4 * i));
    }
    pmsg_put(" wit");
    for (unsigned i = 0; i < 10; ++i) {
        pmsg_put(" ");
        pmsg_hex32(*(volatile uint32_t *)(0xFED17F70UL + 4 * i));
    }
    pmsg_put(" rwd1");
    for (unsigned i = 0; i < 17; ++i) {
        pmsg_put(" ");
        pmsg_hex32(*(volatile uint32_t *)(RWD1_BASE + 4 * i));
    }
    pmsg_put(" p3");
    for (unsigned i = 0; i < 8; ++i) {
        pmsg_put(" ");
        pmsg_hex32(*(volatile uint32_t *)(P3_BASE + 4 * i));
    }
    pmsg_put("\n");
}

/*
 * Firmware<->Windows watchdog/startup handshake, done here because our
 * bootshim + PeilessSec replace the release's, which do it on the S9+:
 *  - the release bootshim (disassembled, 480 B) writes the RWD1 record at
 *    0xFED13D80 as BOOTSHIM_ARMED (uncommitted), sets the entry cookie 'VALY'
 *    at +0x40 and clears PMU INFORM2/INFORM3/SYSIP_DAT0; the release SEC then
 *    commits it as SEC_ACTIVE, and the release DXE/BDS/EBS stages and finally
 *    Exynos9810Ufs.sys (UfsRwd1TakeOwnership: From must be SEC..EBS, record
 *    structurally valid) carry it to WINDOWS_OWNED, after which the driver
 *    takes over the firmware-armed hardware watchdog and acknowledges P3.
 *    Without RWD1 the driver sets NoPetReason=WATCHDOG_INVALID and does none
 *    of that (observed: RWD1 all 0xFF, P3 stuck at PREBRANCH).
 *  - the S9+ installer (BootRouteService) zeroes the P3 'SMP1' record at
 *    0xFED13E80 before every boot; a stale one is left by the previous run.
 * Record format and checksum: drivers/include/Rwd1Constants.h and
 * UfsRwd1CommitLocked/UfsRwd1StructurallyValid in Exynos9810Ufs.c.
 */
static void
star_rwd1_arm(void)
{
    volatile uint32_t *r = (volatile uint32_t *)RWD1_BASE;
    volatile uint32_t *p3 = (volatile uint32_t *)P3_BASE;
    volatile uint32_t *pmu = (volatile uint32_t *)0x14060000UL;
    uint32_t w[16];
    uint32_t acc = 0xA5A55A5AU;                 /* RWD1_CHECKSUM_SEED */

    for (unsigned i = 0; i < 32; ++i)
        p3[i] = 0;
    pmu[0x808 / 4] = 0;                         /* INFORM2 */
    pmu[0x80C / 4] = 0;                         /* INFORM3 */
    pmu[0x810 / 4] = 0;                         /* SYSIP_DAT0 */

    w[0] = 0x31445752U;                         /* 'RWD1' */
    w[1] = (0x40U << 16) | 1U;                  /* RWD1_VERSION_LENGTH */
    w[2] = 1;                                   /* generation */
    w[3] = ~w[2];
    w[4] = 0x20;                                /* RWD1_STATE_SEC_ACTIVE */
    w[5] = 2;                                   /* RWD1_OWNER_SEC */
    w[6] = 2;                                   /* RWD1_PHASE_SEC_ENTRY */
    w[7] = 0;                                   /* RWD1_REASON_NONE */
    w[8] = pmu[0x404 / 4];                      /* RST_STAT */
    w[9] = 0;                                   /* detail */
    w[10] = 0;                                  /* heartbeat */
    w[11] = ~w[10];
    for (unsigned i = 0; i < 12; ++i)
        acc ^= w[i];
    w[12] = acc;
    w[13] = ~acc;
    w[14] = 0x21445752U;                        /* RWD1_COMMIT_MAGIC */
    w[15] = ~w[14];

    /* Commit order of UfsRwd1CommitLocked: commit word cleared first, body,
     * then the inverted commit, then the commit word. */
    r[14] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    for (unsigned i = 0; i < 14; ++i)
        r[i] = w[i];
    __asm__ volatile("dsb sy" : : : "memory");
    r[15] = w[15];
    __asm__ volatile("dsb sy" : : : "memory");
    r[14] = w[14];
    r[16] = 0x594C4156U;                        /* RWD1_ENTRY_COOKIE_VALUE 'VALY' */
    __asm__ volatile("dsb sy" : : : "memory");
    clean_bytes(RWD1_BASE, P3_BASE + 0x80);
    pmsg_put("STARBITE: rwd1 SEC_ACTIVE gen 1, p3 cleared\n");
}

/* Copy XNU's segments, ADT and boot_args into its window; returns the entry PA or 0. */
static uintptr_t
xnu_enter(volatile uint64_t *log, const struct payload *p, uintptr_t rd_start, uint64_t length,
    const char *cmdline, unsigned cmdline_cap)
{
    log[11] = rd_start;
    log[12] = rd_start + length;
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
    /* The header fields up to CommandLine are the same in both layouts (asserted above). */
    volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = ba_pa + PAGE_SIZE;
    ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    if (p->version >= 2) {
        /* 1024 layout: before this, bootFlags and memSizeActual landed inside the kernel's CommandLine
         * (after its NUL) and the kernel read zeros for both (open since 2026-10-28) */
        volatile struct boot_args_1024 *bl = (volatile struct boot_args_1024 *)ba_pa;
        unsigned n = cmdline_cap < XNU_LINE_MAX ? cmdline_cap : XNU_LINE_MAX;
        for (unsigned i = 0; i < n; ++i)
            bl->CommandLine[i] = cmdline[i];
        bl->CommandLine[XNU_LINE_MAX - 1] = 0;
        bl->bootFlags = 0;
        bl->memSizeActual = p->mem_size;
    } else {
        unsigned n = cmdline_cap < BOOT_LINE_LENGTH ? cmdline_cap : BOOT_LINE_LENGTH;
        for (unsigned i = 0; i < n; ++i)
            ba->CommandLine[i] = cmdline[i];
        ba->CommandLine[BOOT_LINE_LENGTH - 1] = 0;
        ba->memSizeActual = p->mem_size;
    }
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

/*
 * XNU: the payload is BOOT's ("@boot", with the boot-args the menu and Engineer edited) or a store
 * entry loaded to STAGE (with its own boot-args); the entry's cmdline is appended to either.
 */
static uintptr_t
boot_xnu(volatile uint64_t *log, const struct cat_ent *e, const char *edited)
{
    static char cmd[XNU_LINE_MAX];
    struct src s;
    uintptr_t rd_start;
    uint64_t length;
    if (!resolve(e->file[0], &s))
        return fail(log, -1, "NO PAYLOAD");
    if (s.kind == SRC_BOOT) {
        rd_start = s.addr;
        length = s.size;
    } else if (s.kind == SRC_STORE) {
        star_fb_text(1, "LOADING XNU");
        stage_file(0, 4, "Reading kernelcache", &s.e);
        int r = store_load(&s.e, STAGE, STAGE_END - STAGE);
        if (r)
            return fail(log, -7, r == -3 ? "PAYLOAD TOO LARGE" : r == -2 ? "PAYLOAD CRC MISMATCH" : "PAYLOAD READ FAILED");
        rd_start = STAGE;
        length = s.e.size;
    } else {
        return fail(log, -1, "NOT AN XNU PAYLOAD");
    }
    const struct payload *p = (const struct payload *)rd_start;
    unsigned ns = s.kind == SRC_STORE ? 4 : 3, k = ns - 3;
    stage_text(k++, ns, "Verifying image", s.kind == SRC_BOOT ? "BOOT payload" : "SWEETXNU", length);
    const char *bad = payload_bad(p, length);
    if (bad)
        return fail(log, -2, bad);
    const char *src = s.kind == SRC_BOOT ? edited : p->cmdline;
    /* a version-2 payload's kernel takes 1023 bytes (XNU_LINE_MAX), the rest 607 */
    unsigned cap = p->version >= 2 ? XNU_LINE_MAX : BOOT_LINE_LENGTH;
    for (unsigned i = 0; i < XNU_LINE_MAX; ++i)
        cmd[i] = i < BOOT_LINE_LENGTH ? src[i] : 0;
    cmd[BOOT_LINE_LENGTH - 1] = 0;
    line_append(cmd, cap, e->cmdline);
    if (launch.xfrag[0])     /* the log level's fragment ("!-v" removes -v) and sbwdt= */
        (void)sb_line_edit(cmd, cap, launch.xfrag, 1);
    /* The boot-args hold cap - 1 bytes and sb_line_edit drops what does not fit: a watchdog whose
     * sbwdt=N did not make it in is not armed (0399's no-sbwdt fallback is only a safety net). */
    if (launch.wdt && !sb_wdt_args_ok(cmd, cap, SB_WDT_XNU, launch.wdt))
        launch.wdt = wdt_dropped(log, e->key, "sbwdt=", launch.wdt);
    {
        struct pline pl;
        pl.n = 0;
        pl_u(&pl, p->nsegs);
        pl_s(&pl, " segments");
        star_menu_progress(k++, ns, "Copying kernel", pl.b);
    }
    uintptr_t entry = xnu_enter(log, p, rd_start, length, cmd, cap);
    if (entry)
        stage_entry(k, ns, "Handing off to XNU", entry);
    return entry;
}

static uintptr_t
boot_entry(unsigned i, volatile uint64_t *log, uintptr_t fdt, const char *cmdline)
{
    const struct cat_ent *e = cat_get(i);
    const char *why;
    boot_why = 0;
    /* no USB DMA (a download the Mac had under way) into STAGE while this start loads there; a
     * start that fails comes back to a menu, whose next poll brings it up again (fbbg.h) */
    fbbg_stop();
    launch_clear();
    if (!e)
        return fail(log, -12, "NO SUCH ENTRY");
    if (!star_entry_ready(i, &why))
        return fail(log, -13, why);
    e = launch_prepare(e, log);
    /* log level 0: none of the loader's own status lines; 3: all of them, even under menu v7 */
    int was = star_fb_muted();
    if (launch.lvl == 0)
        star_fb_mute(1);
    else if (launch.lvl >= 3)
        star_fb_mute(0);
    uintptr_t entry;
    if (e->method == M_XNU)
        entry = boot_xnu(log, e, cmdline);
    else if (e->method == M_UEFI)
        entry = boot_uefi(log, fdt, e);
    else if (e->method == M_LINUX)
        entry = boot_linux(log, e);
    else if (e->method == M_ANDROID)
        entry = boot_android(log, fdt, e);
    else
        entry = boot_bare(log, fdt, e);
    if (!entry) {
        star_fb_mute(was);
        launch_clear();
    }
    return entry;
}

/* ---- boot setup, Starbite Recovery and Rescue (recovery.c, docs/STARBITE-RECOVERY.md) ------- */

static uintptr_t boot_fdt;
static uint64_t boot_el, boot_sctlr;
static struct star_info boot_in;
static char boot_cmdline[BOOT_LINE_LENGTH];

/* BOOT's payload and bundles (RAM only: what S-Boot loaded), the star_info the menus get and the
 * iOS boot-args (filled once: Engineer edits the boot-args in place). */
static void boot_info(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    boot_payload(boot_fdt);
    boot_in.el = boot_el;
    boot_in.sctlr = boot_sctlr;
    boot_in.fdt = boot_fdt;
    boot_in.rd_start = boot_rd_start;
    boot_in.rd_end = boot_rd_end;
    boot_in.phys_base = boot_p ? boot_p->phys_base : 0;
    boot_in.mem_size = boot_p ? boot_p->mem_size : 0;
    boot_in.entry_va = boot_p ? boot_p->entry_va : 0;
    boot_in.nsegs = boot_p ? boot_p->nsegs : 0;
    for (unsigned i = 0; i < BOOT_LINE_LENGTH; ++i)
        boot_cmdline[i] = boot_p ? boot_p->cmdline[i] : 0;
    boot_cmdline[BOOT_LINE_LENGTH - 1] = 0;
}

/* What star_splash records for the menus (the SBMENU01 sprites and the font atlas after them),
 * with nothing drawn: Engineer Lite from the recovery menu needs the atlas, not the intro. */
static void assets_find(void)
{
    uint64_t start, end;
    uintptr_t fdt = boot_fdt;
    if (!fdt || (fdt & 7) || !fdt_chosen_u64(fdt, "linux,initrd-start", &start) ||
        !fdt_chosen_u64(fdt, "linux,initrd-end", &end) || end <= start + 16)
        return;
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)start;
    if (h[0] == 0x454d4253U && h[1] == 0x3130554eU && sprite_walk(start, end, 0xffffffffU) >= 0) {
        menu_start = start;
        menu_end = end;
    }
}

const struct star_info *star_boot_setup(char **cmdline, unsigned *len)
{
    assets_find();
    boot_info();
    if (cmdline)
        *cmdline = boot_cmdline;
    if (len)
        *len = BOOT_LINE_LENGTH;
    return &boot_in;
}

/* Why no OS may start from this state, or 0. */
static const char *boot_env_bad(void)
{
    if (boot_el != 4)
        return "NOT EL1";
    if (boot_sctlr & 1)
        return "MMU ALREADY ON";
    return 0;
}

/*
 * Starbite Rescue: a Linux kernel + DTB + rescue initrd (tools/rescue/), found without the catalog
 * or the menu:
 *  1. an "SBRESCUE" bundle in BOOT (RAM only): the bundle page (u64 magic, u64 size), then
 *     "SBRSCU01", u32 version 1, u32 flags, three { u64 offset, u64 size, u64 CRC32 } (Image,
 *     DTB, initrd; offsets from the data start, 4 KiB aligned), char cmdline[256] at +88
 *     (tools/rescue/bundle-rescue.py);
 *  2. else the OS store: rescue.initrd with rescue.image|linux.image and rescue.dtb|linux.dtb.
 * The kernel command line gets "sb.rescue=1" (and the bundle's cmdline).
 */
#define RESCUE_MAGIC 0x3130554353524253ULL   /* "SBRSCU01" */
#define RESCUE_CMDLINE "sb.rescue=1"

struct rescue_part {
    uintptr_t at;
    uint64_t size;
    uint32_t crc;
};

/* The bundle's three parts, bounds and CRCs checked: 1, or 0 with *why (0 when there is none). */
static int rescue_bundle(struct rescue_part part[3], const char **cmd, const char **why)
{
    uint64_t size;
    *why = 0;
    uintptr_t b = find_bundle(BUNDLE_RESCUE, &size);
    if (!b)
        return 0;
    const volatile uint64_t *h = (const volatile uint64_t *)b;
    if (size < 4096 || h[0] != RESCUE_MAGIC || (uint32_t)h[1] != 1) {
        *why = "BAD RESCUE BUNDLE HEADER";
        return 0;
    }
    for (unsigned k = 0; k < 3; ++k) {
        uint64_t off = h[2 + 3 * k], len = h[3 + 3 * k];
        if (off < 4096 || (off & (PAGE_SIZE - 1)) || off > size || !len || len > size - off) {
            *why = "BAD RESCUE BUNDLE LAYOUT";
            return 0;
        }
        part[k].at = b + off;
        part[k].size = len;
        part[k].crc = (uint32_t)h[4 + 3 * k];
    }
    *cmd = (const char *)(b + 88);
    if (part[0].size < 64 || *(volatile const uint32_t *)(part[0].at + 56) != 0x644d5241U) {
        *why = "RESCUE KERNEL IS NOT AN ARM64 IMAGE";
        return 0;
    }
    for (unsigned k = 0; k < 3; ++k)
        if (star_crc32(part[k].at, part[k].size) != part[k].crc) {
            *why = k == 0 ? "RESCUE KERNEL CRC MISMATCH" : k == 1 ? "RESCUE DTB CRC MISMATCH" : "RESCUE INITRD CRC MISMATCH";
            return 0;
        }
    return 1;
}

static const struct cat_ent rescue_ent = {
    .key = "rescue",
    .name = "Starbite Rescue",
    .file = { "rescue.image|linux.image", "rescue.dtb|linux.dtb", "rescue.initrd", "" },
    .cmdline = RESCUE_CMDLINE,
    .category = CAT_STARBITE,
    .method = M_LINUX,
};

int star_rescue_ready(const char **why)
{
    struct rescue_part part[3];
    const char *cmd, *w;
    struct src s;
    boot_info();
    if (rescue_bundle(part, &cmd, &w))
        return 1;
    if (w) {
        *why = w;
        return 0;
    }
    if (!resolve(rescue_ent.file[2], &s) || s.kind != SRC_STORE)
        *why = "NO RESCUE BUNDLE IN BOOT, NO RESCUE.INITRD IN THE STORE";
    else if (!resolve(rescue_ent.file[0], &s) || s.kind != SRC_STORE)
        *why = "NO RESCUE KERNEL IN THE STORE";
    else if (!resolve(rescue_ent.file[1], &s) || s.kind != SRC_STORE)
        *why = "NO RESCUE DTB IN THE STORE";
    else
        return 1;
    return 0;
}

/* The rescue system from BOOT's bundle: as boot_linux, from RAM. */
static uintptr_t rescue_from_bundle(volatile uint64_t *log, const struct rescue_part part[3], const char *bcmd)
{
    static char cmd[320];
    static char extra[256];
    unsigned n = 0;
    for (const char *c = RESCUE_CMDLINE; *c; ++c)
        cmd[n++] = *c;
    cmd[n] = 0;
    for (unsigned k = 0; k < 255; ++k)
        extra[k] = *(volatile const char *)(bcmd + k);
    extra[255] = 0;
    line_append(cmd, sizeof(cmd), extra);
    const struct rescue_part *img = &part[0], *dtb = &part[1], *rd = &part[2];
    uint64_t text_offset = *(volatile const uint64_t *)(img->at + 8);
    uint64_t span = *(volatile const uint64_t *)(img->at + 16);
    if (span < img->size)
        span = img->size;
    uintptr_t dst = LINUX_BASE + text_offset;
    if (text_offset >= (2UL << 20) || span > OS_RD - dst)
        return fail(log, -71, "RESCUE KERNEL TOO LARGE");
    if (dtb->size > (2UL << 20))
        return fail(log, -72, "RESCUE DTB TOO LARGE");
    if (rd->size > EDIT_FDT - OS_RD)
        return fail(log, -73, "RESCUE INITRD TOO LARGE");
    star_fb_text(2, "COPYING THE RESCUE SYSTEM");
    copy_bytes(EDIT_FDT, dtb->at, dtb->size);
    uint32_t dsize = fdt_copy_append_bootargs(EDIT_FDT, LINUX_DTB, (uint32_t)(UFS_DMA - LINUX_DTB), cmd);
    if (!dsize)
        return fail(log, -74, "RESCUE DTB HAS NO BOOTARGS");
    uint64_t now;
    if (em_rtc_epoch(&now))
        (void)fdt_chosen_set_u64(LINUX_DTB, "starbite,rtc-epoch", now);
    copy_bytes(OS_RD, rd->at, rd->size);
    if (!fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-start", OS_RD) ||
        !fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-end", OS_RD + rd->size))
        return fail(log, -75, "RESCUE DTB HAS NO INITRD PROPERTIES");
    copy_bytes(dst, img->at, img->size);
    uint8_t l31 = 0, l2 = 0;
    (void)hw_sd_rails(&l31, &l2);         /* a STARBITE-RESCUE microSD works too (tools/rescue/init) */
    log[18] = (uint64_t)l31 << 8 | l2;
    clean_bytes(dst, dst + span);
    clean_bytes(OS_RD, OS_RD + rd->size);
    clean_bytes(LINUX_DTB, LINUX_DTB + dsize);
    log[10] = 0x700;
    log[15] = dst;
    log[17] = LINUX_DTB;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/* Rescue mode after the reboot the recovery menu asked for: the entry PA, or 0 with boot_why. */
static uintptr_t star_rescue_boot(volatile uint64_t *log)
{
    struct rescue_part part[3];
    const char *cmd, *w;
    boot_why = 0;
    launch_clear();     /* no launch record: rescue mode must not need UFS */
    star_fb_row_base(0);
    star_fb_text_c(0, "STARBITE RESCUE MODE", 0xffffd43bU, 0xff000000U);
    star_fb_text(1, "STARTING THE RESCUE SYSTEM");
    pmsg_put("STARBITE: rescue mode\n");
    if (rescue_bundle(part, &cmd, &w)) {
        pmsg_put("STARBITE: rescue from the BOOT bundle\n");
        return rescue_from_bundle(log, part, cmd);
    }
    if (w)
        return fail(log, -70, w);
    if (!star_rescue_ready(&w))
        return fail(log, -70, w);
    pmsg_put("STARBITE: rescue from the store\n");
    uintptr_t entry = boot_linux(log, &rescue_ent);
    if (entry)
        log[10] = 0x701;
    return entry;
}

/*
 * ---- Starbite Maintenance (docs/MAINTENANCE.md 4.2: R2, R4, R5) ------------------------------
 * Sources, in order: the GPT partition "SBMAINT" (two SBMAINT1 slots: A at byte 0, B at half the
 * partition rounded down to 1 MiB; the valid one with the higher seq first, "maint-prev" the other
 * way round; a slot is read whole into STAGE and every section's CRC checked before anything is
 * placed), else the catalog entry "maint" ("maint-rescue") from the OS store, else nothing.
 * The start is the rescue start's: kernel at RAM_BASE + text_offset, initrd at OS_RD, the DTB (or
 * dtb-rescue) at LINUX_DTB with " <header cmdline> sb.maint=1 sb.maint.src=a|b sb.maint.seq=N
 * sb.maint.from=next|fastboot [sb.rescue=1] sb.serial=<androidboot.serialno>" appended, the microSD
 * rails on. Log slot 10: 0x710 slot A, 0x711 slot B, 0x712 the store, 0x713 `fastboot oem maint`.
 */
#define MAINT_FROM_NEXT     0U
#define MAINT_FROM_FASTBOOT 1U

struct mslot {
    int valid;
    uint64_t lba, bytes;          /* the slot's first 4 KiB block on LU0, its size */
    struct sbm_info m;
    const char *why;
};
static struct mslot mslots[2];
static char maint_why[160];

/* The two slots' headers (GPT + two block reads): 1 if the partition exists. */
static int maint_scan(void)
{
    uint64_t first, last;
    for (unsigned i = 0; i < 2; ++i) {
        mslots[i].valid = 0;
        mslots[i].why = "NO SBMAINT PARTITION";
    }
    if (!ufs_up() || !gpt_find("SBMAINT", &first, &last) || last < first)
        return 0;
    uint64_t bytes = (last - first + 1U) * 4096U, b = sbm_slot_b(bytes);
    static uint8_t h[SBM_HDR];
    for (unsigned i = 0; i < 2; ++i) {
        struct mslot *s = &mslots[i];
        uint64_t off = i ? b : 0;
        s->lba = first + off / 4096U;
        s->bytes = i ? bytes - b : b;
        if (!s->bytes || ufs_read(s->lba, 1)) {
            s->why = "UNREADABLE";
            continue;
        }
        uint8_t *hp = h;
        __asm__("" : "+r"(hp));   /* no folded "h - UFS_BUF" constant: that is an ABS64 relocation */
        for (unsigned k = 0; k < SBM_HDR; ++k)
            hp[k] = *(volatile uint8_t *)(UFS_BUF + k);
        s->valid = sbm_check(h, s->bytes, &s->m, &s->why);
    }
    return 1;
}

static void why_cat(const char *s)
{
    unsigned n = 0;
    while (n < sizeof(maint_why) - 1 && maint_why[n])
        ++n;
    for (; s && *s && n < sizeof(maint_why) - 1; ++s)
        maint_why[n++] = *s;
    maint_why[n] = 0;
}

/* The catalog entry for this variant: index, or -1; *add_rescue when "maint" stands in. */
static int maint_entry(unsigned variant, int *add_rescue)
{
    *add_rescue = 0;
    int i = variant == MAINT_RESCUE ? cat_find("maint-rescue") : -1;
    if (i < 0 || !star_entry_ready((unsigned)i, 0)) {
        i = cat_find("maint");
        *add_rescue = variant == MAINT_RESCUE;
    }
    return i >= 0 && star_entry_ready((unsigned)i, 0) ? i : -1;
}

static const char *maint_missing(int part)
{
    maint_why[0] = 0;
    if (part) {
        why_cat("SLOT A ");
        why_cat(mslots[0].why);
        why_cat(", SLOT B ");
        why_cat(mslots[1].why);
    } else {
        why_cat("NO SBMAINT PARTITION");
    }
    why_cat(", NO MAINT ENTRY IN THE STORE");
    return maint_why;
}

int star_maint_ready(const char **why)
{
    int add;
    int part = maint_scan();
    if (mslots[0].valid || mslots[1].valid || maint_entry(MAINT_NORMAL, &add) >= 0)
        return 1;
    *why = maint_missing(part);
    return 0;
}

/* " name=value" from S-Boot's bootargs (sb.serial) */
static void sboot_arg(const char *key, char *out, unsigned cap)
{
    uintptr_t v;
    uint32_t len;
    out[0] = 0;
    if (boot_fdt && !(boot_fdt & 7) && fdt_chosen_raw(boot_fdt, "bootargs", &v, &len))
        (void)sb_arg((const char *)v, len, key, out, cap);
}

/* slot: "a", "b", or 0 for the store (whose catalog cmdline has sb.maint=1 sb.maint.src=store) */
static void maint_extra(char *cmd, unsigned cap, const char *slot, uint32_t seq, unsigned from, int rescue)
{
    char t[64], serial[40];
    unsigned n = 0;
    const char *a;
    if (slot) {
        for (a = "sb.maint=1 sb.maint.src="; *a; ++a)
            t[n++] = *a;
        for (; *slot; ++slot)
            t[n++] = *slot;
        t[n] = 0;
        line_append(cmd, cap, t);
    }
    if (seq) {
        char d[12];
        unsigned k = 0;
        do {
            d[k++] = (char)('0' + seq % 10U);
            seq /= 10U;
        } while (seq);
        n = 0;
        for (a = "sb.maint.seq="; *a; ++a)
            t[n++] = *a;
        while (k)
            t[n++] = d[--k];
        t[n] = 0;
        line_append(cmd, cap, t);
    }
    line_append(cmd, cap, from == MAINT_FROM_FASTBOOT ? "sb.maint.from=fastboot" : "sb.maint.from=next");
    if (rescue)
        line_append(cmd, cap, "sb.rescue=1");
    sboot_arg("androidboot.serialno", serial, sizeof(serial));
    if (serial[0]) {
        n = 0;
        for (a = "sb.serial="; *a; ++a)
            t[n++] = *a;
        for (unsigned k = 0; serial[k] && n + 1 < sizeof(t); ++k)
            t[n++] = serial[k];
        t[n] = 0;
        line_append(cmd, cap, t);
    }
}

/* Slot i (already scanned and valid) read whole into STAGE, checked, placed: the entry, or 0 and
 * *why (then the next slot is tried). */
static uintptr_t maint_from_slot(volatile uint64_t *log, unsigned i, unsigned variant, unsigned from, const char **why)
{
    const struct mslot *s = &mslots[i];
    const struct sbm_info *m = &s->m;
    static char cmd[512];
    if (ufs_read_to(s->lba, m->total / 4096U, STAGE)) {
        *why = "SLOT READ FAILED";
        return 0;
    }
    for (unsigned k = 0; k < 4; ++k)
        if (m->size[k] && star_crc32(STAGE + m->off[k], m->size[k]) != m->crc[k]) {
            *why = k == SBM_KERNEL ? "KERNEL CRC" : k == SBM_INITRD ? "INITRD CRC" : "DTB CRC";
            return 0;
        }
    uintptr_t k0 = STAGE + m->off[SBM_KERNEL];
    if (*(volatile uint32_t *)(k0 + 56) != 0x644d5241U) {
        *why = "KERNEL NOT ARM64";
        return 0;
    }
    uint64_t text_offset = *(volatile uint64_t *)(k0 + 8), span = *(volatile uint64_t *)(k0 + 16);
    if (span < m->size[SBM_KERNEL])
        span = m->size[SBM_KERNEL];
    uintptr_t dst = LINUX_BASE + text_offset;
    if (text_offset >= (2UL << 20) || span > OS_RD - dst) {
        *why = "KERNEL TOO LARGE";
        return 0;
    }
    if (m->size[SBM_INITRD] > EDIT_FDT - OS_RD) {
        *why = "INITRD TOO LARGE";
        return 0;
    }
    unsigned d = variant == MAINT_RESCUE && m->size[SBM_DTB_RESCUE] ? SBM_DTB_RESCUE : SBM_DTB;
    /* the DTB first: its failures are the cheap ones */
    copy_bytes(EDIT_FDT, STAGE + m->off[d], m->size[d]);
    cmd[0] = 0;
    {
        unsigned n = 0;
        for (; n + 1 < sizeof(cmd) && n < sizeof(m->cmdline) && m->cmdline[n]; ++n)
            cmd[n] = m->cmdline[n] >= 32 && m->cmdline[n] < 127 ? m->cmdline[n] : ' ';
        cmd[n] = 0;
    }
    maint_extra(cmd, sizeof(cmd), i ? "b" : "a", m->seq, from, variant == MAINT_RESCUE);
    uint32_t dsize = fdt_copy_append_bootargs(EDIT_FDT, LINUX_DTB, (uint32_t)(UFS_DMA - LINUX_DTB), cmd);
    if (!dsize) {
        *why = "DTB HAS NO BOOTARGS";
        return 0;
    }
    uint64_t now;
    if (em_rtc_epoch(&now))
        (void)fdt_chosen_set_u64(LINUX_DTB, "starbite,rtc-epoch", now);
    copy_bytes(OS_RD, STAGE + m->off[SBM_INITRD], m->size[SBM_INITRD]);
    if (!fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-start", OS_RD) ||
        !fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-end", OS_RD + m->size[SBM_INITRD])) {
        *why = "DTB HAS NO INITRD PROPERTIES";
        return 0;
    }
    copy_bytes(dst, k0, m->size[SBM_KERNEL]);
    uint8_t l31 = 0, l2 = 0;
    (void)hw_sd_rails(&l31, &l2);
    log[18] = (uint64_t)l31 << 8 | l2;
    {
        struct pline p;
        p.n = 0;
        pl_s(&p, "STARTING SLOT ");
        pl_s(&p, i ? "B" : "A");
        pl_s(&p, " (SEQ ");
        pl_u(&p, m->seq);
        pl_s(&p, ")");
        star_fb_text_c(1, p.b, 0xffffffffU, 0xff000000U);
    }
    {
        struct pline p;
        p.n = 0;
        pl_s(&p, "STARBITE: maintenance from slot ");
        pl_s(&p, i ? "b" : "a");
        pl_s(&p, " seq ");
        pl_u(&p, m->seq);
        pl_s(&p, " ");
        pmsg_put(p.b);
        pmsg_put(m->version);
        pmsg_put("\n");
    }
    clean_bytes(dst, dst + span);
    clean_bytes(OS_RD, OS_RD + m->size[SBM_INITRD]);
    clean_bytes(LINUX_DTB, LINUX_DTB + dsize);
    log[10] = from == MAINT_FROM_FASTBOOT ? 0x713 : 0x710 + i;
    log[15] = dst;
    log[17] = LINUX_DTB;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/* Starbite Maintenance from the best source: the entry PA, or 0 with boot_why. */
static uintptr_t star_maint_boot(volatile uint64_t *log, unsigned variant, unsigned from)
{
    boot_why = 0;
    star_fb_row_base(0);
    star_fb_text_c(0, "STARBITE MAINTENANCE", 0xffffd43bU, 0xff000000U);
    star_fb_text_c(1, "LOOKING FOR IT...", 0xffb1bad3U, 0xff000000U);
    pmsg_put(variant == MAINT_PREV ? "STARBITE: maintenance (other slot)\n" :
             variant == MAINT_RESCUE ? "STARBITE: maintenance (rescue)\n" : "STARBITE: maintenance\n");
    launch_set("maint", "Starbite Maintenance", M_LINUX, CLF_MAINT | (from == MAINT_FROM_FASTBOOT ? CLF_FASTBOOT : 0U));
    int part = maint_scan();
    int valid[2] = { mslots[0].valid, mslots[1].valid };
    uint32_t seq[2] = { mslots[0].m.seq, mslots[1].m.seq };
    unsigned order[2];
    unsigned n = sbm_order(valid, seq, variant == MAINT_PREV, order);
    for (unsigned k = 0; k < n; ++k) {
        const char *w = 0;
        uintptr_t e = maint_from_slot(log, order[k], variant, from, &w);
        if (e)
            return e;
        mslots[order[k]].why = w;
        mslots[order[k]].valid = 0;
    }
    int add_rescue;
    int i = maint_entry(variant, &add_rescue);
    if (i >= 0) {
        const struct cat_ent *e = cat_get((unsigned)i);
        const uint8_t *src = (const uint8_t *)e;
        uint8_t *dst = (uint8_t *)&launch_ent;
        for (unsigned b = 0; b < sizeof(launch_ent); ++b)
            dst[b] = src[b];
        maint_extra(launch_ent.cmdline, sizeof(launch_ent.cmdline), 0, 0, from, add_rescue);
        star_fb_text_c(1, "STARTING FROM THE OS STORE", 0xffffffffU, 0xff000000U);
        pmsg_put("STARBITE: maintenance from the store\n");
        uintptr_t entry = boot_linux(log, &launch_ent);
        if (entry) {
            log[10] = from == MAINT_FROM_FASTBOOT ? 0x713 : 0x712;
            clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
            return entry;
        }
        launch_clear();
        return 0;
    }
    launch_clear();
    maint_missing(part);
    static char full[200];
    unsigned m = 0;
    for (const char *a = "NO MAINTENANCE SYSTEM: "; *a; ++a)
        full[m++] = *a;
    for (unsigned k = 0; maint_why[k] && m + 1 < sizeof(full); ++k)
        full[m++] = maint_why[k];
    full[m] = 0;
    return fail(log, -80, full);
}

/* `fastboot oem maint [prev|rescue]` (fastboot_dev.c): placed and ready, or 0 with *why. */
uintptr_t star_maint_ram(unsigned variant, const char **why)
{
    uintptr_t e = star_maint_boot((volatile uint64_t *)LOG_BASE, variant, MAINT_FROM_FASTBOOT);
    *why = e ? 0 : boot_why;
    return e;
}

/* The recovery menu until it says Continue: an entry PA to start, or 0 to go on booting. */
static uintptr_t recovery_loop(volatile uint64_t *log, const char *why)
{
    for (;;) {
        int pick = -1;
        int r = recovery_main(why, &pick);
        if (r == REC_USBBOOT)               /* the Mac placed an image while the menu waited */
            return fbbg_entry();
        if (r == REC_FASTBOOT) {
            /* USB fastboot / console from recovery: `fastboot boot` starts its image; any other
             * way out (continue, Bixby) comes back to the recovery menu. */
            star_boot_setup(0, 0);
            rec_panel_lost();
            star_fb_row_base(0);
            if (fastboot_run() == FASTBOOT_BOOT)
                return fastboot_entry();
            why = 0;
            continue;
        }
        if (r != REC_BOOT)
            return 0;
        const char *bad = boot_env_bad();
        if (bad) {
            why = bad;
            continue;
        }
        star_boot_setup(0, 0);
        rec_panel_lost();       /* a load may overwrite the panel copy */
        star_fb_row_base(0);
        uintptr_t entry = boot_entry((unsigned)pick, log, boot_fdt, boot_cmdline);
        if (entry)
            return entry;
        why = boot_why ? boot_why : "COULD NOT START";
    }
}

/* Status stays in the log page at LOG_BASE. */
static uintptr_t prepare(uintptr_t fdt, volatile uint64_t *log, uint64_t el, uint64_t sctlr);

/* Returns the entry PA, or 0 for Download mode. A start ends with launch_go: the launch record and
 * the watchdog, the last things before xnu_boot.S leaves the loader. */
uintptr_t star_prepare_xnu(uintptr_t fdt, volatile uint64_t *log, uint64_t el, uint64_t sctlr)
{
    uintptr_t entry = prepare(fdt, log, el, sctlr);
    /* whatever starts now (an OS, or S-Boot's Download mode for 0) sets USB up itself: the
     * menus' background fastboot stops, the DWC3 soft-reset (usbd_stop) */
    fbbg_off();
    if (entry) {
        launch_go(log);
        clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    }
    return entry;
}

static uintptr_t prepare(uintptr_t fdt, volatile uint64_t *log, uint64_t el, uint64_t sctlr)
{
    /* Caches on before anything is drawn (mmu.c); the checks below see S-Boot's sctlr. */
    if (el == 4 && !(sctlr & 1))
        star_mmu_enable();
    log[77] = star_fb_enable();
    /* The Bixby window: only the key GPIO, the timer and fbtext up to here (recovery.c). */
    int rec = rec_bixby_window(REC_WINDOW_MS);
    /* What the last OS left in the ramoops area, before the first pmsg line changes it: reads and
     * a RAM copy only (crash.c). */
    crash_snapshot(fdt);
    boot_fdt = fdt;
    boot_el = el;
    boot_sctlr = sctlr;
    star_dump_latches();
    /* The boot-next keys the loader itself takes (mailbox cleared at once): rescue, and Starbite
     * Maintenance's maint / maint-prev / maint-rescue (docs/MAINTENANCE.md 4.2 R2). */
    int next = rec_next_take();
    int rescue = next == REC_NEXT_RESCUE;
    uintptr_t entry;
    if (rec) {
        pmsg_put("STARBITE: Bixby held: Starbite Recovery\n");
        if (next != REC_NEXT_NONE)
            pmsg_put("STARBITE: Bixby cancelled the boot-next request\n");
        rescue = 0;
        next = REC_NEXT_NONE;
        entry = recovery_loop(log, 0);
        if (entry)
            return entry;
    }
    int branded = star_splash(fdt);
    if (el != 4)
        return fatal(log, -10, "NOT EL1");
    if (sctlr & 1)
        return fatal(log, -11, "MMU ALREADY ON");
    boot_info();
    char *cmdline = boot_cmdline;
#ifdef STAR_CHAIN_AUTOBOOT
    /*
     * A chained test loader (tools/menu/chain, built with EXTRA_CFLAGS=-DSTAR_CHAIN_AUTOBOOT, e.g.
     * tools/apple/m1-chain.sh): BOOT's payload is the one appended to this Image, and it starts at once.
     * No POST, no menu, no launch record, no watchdog of the loader's own and no log fragments: the chain
     * stub's dead-man (pack.py --wdt) keeps running into the OS, which must not feed it (XNU: sbstop=).
     * If the payload cannot start, the normal menu of this loader follows.
     */
    if (boot_p) {
        static struct cat_ent t;
        lscopy(t.key, sizeof(t.key), "chaintest");
        lscopy(t.name, sizeof(t.name), "Chained test payload");
        lscopy(t.file[0], sizeof(t.file[0]), "@boot");
        t.method = M_XNU;
        launch_clear();
        pmsg_put("STARBITE: chain test loader: starting the appended payload (no menu, no launch record)\n");
        entry = boot_xnu(log, &t, boot_cmdline);
        if (entry)
            return entry;
        pmsg_put("STARBITE: chain test loader: the appended payload did not start; menu\n");
    } else {
        pmsg_put("STARBITE: chain test loader: no appended payload; menu\n");
    }
#endif
    log[11] = boot_rd_start;
    log[12] = boot_rd_end;
    /* Rescue mode (the recovery menu's reboot): no POST, no menu. A failure comes back to recovery. */
    if (rescue) {
        entry = star_rescue_boot(log);
        if (entry)
            return entry;
        rec_panel_lost();
        entry = recovery_loop(log, boot_why ? boot_why : "RESCUE MODE FAILED");
        if (entry)
            return entry;
    }
    /* Starbite Maintenance (the recovery menu's reboot, `reboot-to maint`, SBMAINT's own reboots):
     * no POST, no menu; a failure comes back to recovery with the reason. */
    if (next >= REC_NEXT_MAINT) {
        unsigned variant = next == REC_NEXT_MAINT_PREV ? MAINT_PREV : next == REC_NEXT_MAINT_RESCUE ? MAINT_RESCUE
                                                                                                  : MAINT_NORMAL;
        entry = star_maint_boot(log, variant, MAINT_FROM_NEXT);
        if (entry)
            return entry;
        rec_panel_lost();
        entry = recovery_loop(log, boot_why ? boot_why : "MAINTENANCE FAILED");
        if (entry)
            return entry;
    }
    /* POST at the settings record's level (full by default; Volume Down skips it once): a fatal
     * check, or Bixby held during it, brings up the recovery menu. */
    {
        int bixby = 0, skipped = 0;
        const char *why = 0;
        struct star_settings ps;
        if (!settings_load(&ps))
            sbset_defaults(&ps);
        /* The full POST leaves the clean Starbite logo on the panel when menu v7 will start from it
         * (its intro hands the One mark over); log level 2-3 shows the live log from the start. */
        unsigned pf = (branded ? POST_F_TOUCH : 0U) | (branded && menu7_ready() ? POST_F_KEEP_LOGO : 0U) |
                      (sbset_log(&ps) >= SET_LOG_VERBOSE ? POST_F_LOG : 0U);
        int f = post_boot(sbset_post(&ps), pf, &bixby, &skipped, &why);
        if (f || bixby) {
            entry = recovery_loop(log, why);
            if (entry)
                return entry;
        }
    }
    (void)cat_load();
    /* From here every menu that waits for keys (the crash report, the boot menu, Starbite
     * Recovery) also serves USB fastboot, so the Mac can drive it (fbbg.h). It comes up at the
     * first poll of the first of them (after menu v7's intro, which polls nothing: a host would
     * otherwise wait through it in the middle of enumeration). */
    fbbg_enable();
    /* Crash and hang detection: the last launch judged, a report saved, the crash screen
     * (crash.c). Then the menu, Starbite Recovery or the same entry once more (settings). */
    {
        int retry = -1;
        int act = crash_after(&retry);
        if (fbbg_pending())                 /* the Mac placed an image during the crash report */
            return fbbg_entry();
        if (act == CRASH_GO_RECOVERY) {
            entry = recovery_loop(log, "THE LAST START CRASHED");
            if (entry)
                return entry;
        } else if (act == CRASH_GO_RETRY && retry >= 0) {
            launch.retry = 1;
            entry = boot_entry((unsigned)retry, log, fdt, cmdline);
            launch.retry = 0;
            if (entry)
                return entry;
        }
    }
    /* Loader fastboot (FB1, fastboot_dev.c, docs/USB-FASTBOOT.md): Volume Up held, or boot-next
     * "fastboot". `fastboot boot` starts its image from here; `fastboot continue`: the menu. */
    {
        uintptr_t fb = fastboot_hook();
        if (fb)
            return fb;
    }
    /* The boot menu (and Starbite Engineer) on the branded screen; XNU RAM is still free here,
     * for Engineer's RAM test. A start that fails comes back to the menu. Unbranded: iOS. */
    for (;;) {
        int pick = branded ? star_menu(&boot_in, cmdline, BOOT_LINE_LENGTH) : star_ios_pick();
        if (pick == STAR_PICK_USB)          /* the Mac placed an image while the menu waited */
            return fbbg_entry();
        if (pick <= 0)
            return branded ? 0 : fatal(log, -1, "NO PAYLOAD");
        entry = boot_entry((unsigned)(pick - 1), log, fdt, cmdline);
        if (entry)
            return entry;
        if (!branded)
            return fatal(log, 0, 0);
        star_menu_failed(cat_get((unsigned)(pick - 1))->name, boot_why);
    }
}

/*
 * ---- Loader fastboot: an image already in RAM (ramboot.h, fastboot_dev.c) --------------------
 * `fastboot boot` puts the image in STAGE (the download buffer); this places it exactly as the
 * catalog's start path for its method would after loading it from the store, and leaves the
 * same handoff state (log[10] method code, log[15] entry, log[17] x0). XNU goes through the
 * same xnu_enter; the other methods repeat the few placement steps of boot_linux / boot_android
 * / boot_bare / boot_uefi, so those functions stay untouched.
 */
_Static_assert(STAR_RAM_DL == STAGE && STAR_RAM_DL_END == STAGE_END, "fastboot buffer is STAGE");

static uintptr_t ram_kernel(volatile uint64_t *log, const struct star_ram *r, uintptr_t limit,
    int64_t step, uint64_t *span)
{
    if (r->kernel_size < 64 || (r->kernel & 7) || *(volatile uint32_t *)(r->kernel + 56) != 0x644d5241U)
        return fail(log, step, "NOT AN ARM64 IMAGE");
    uint64_t text_offset = *(volatile uint64_t *)(r->kernel + 8);
    uint64_t image_size = *(volatile uint64_t *)(r->kernel + 16);
    if (image_size < r->kernel_size)
        image_size = r->kernel_size;
    uintptr_t dst = LINUX_BASE + text_offset;
    if (text_offset >= (2UL << 20) || image_size > limit - dst)
        return fail(log, step - 1, "KERNEL TOO LARGE");
    star_fb_hex(2, "KERNEL AT", dst);
    copy_bytes(dst, r->kernel, r->kernel_size);
    *span = image_size;
    return dst;
}

static uintptr_t ram_handoff(volatile uint64_t *log, uint64_t code, uintptr_t entry, uintptr_t x0)
{
    log[10] = code;
    log[15] = entry;
    log[17] = x0;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return entry;
}

static uintptr_t ram_start(volatile uint64_t *log, const struct star_ram *r, uintptr_t fdt)
{
    static char cmd[XNU_LINE_MAX];
    uint64_t span;
    uintptr_t dst;
    boot_why = 0;
    star_fb_text(1, "FASTBOOT: STARTING IMAGE");
    if (r->method == M_XNU) {
        const struct payload *p = (const struct payload *)r->kernel;
        const char *bad = (r->kernel & 7) ? "PAYLOAD NOT ALIGNED" : payload_bad(p, r->kernel_size);
        if (bad)
            return fail(log, -71, bad);
        if (r->kernel + r->kernel_size > RAM_BASE)
            return fail(log, -71, "PAYLOAD OVER THE XNU WINDOW");
        unsigned cap = p->version >= 2 ? XNU_LINE_MAX : BOOT_LINE_LENGTH;
        for (unsigned i = 0; i < XNU_LINE_MAX; ++i)
            cmd[i] = i < BOOT_LINE_LENGTH ? p->cmdline[i] : 0;
        cmd[BOOT_LINE_LENGTH - 1] = 0;
        line_append(cmd, cap, r->cmdline);
        return xnu_enter(log, p, r->kernel, r->kernel_size, cmd, cap);
    }
    if (r->method == M_UEFI) {
        uint64_t size = r->kernel_size;
        if (size < 64 || STAGE + size > UEFI_FD)
            return fail(log, -72, "UEFI IMAGE TOO LARGE");
        if (r->kernel != STAGE)
            copy_bytes(STAGE, r->kernel, size);       /* down to STAGE: ascending, overlap-safe */
        if (*(volatile uint32_t *)(STAGE + 56) != 0x644d5241U)
            return fail(log, -72, "BAD WINDOWS UEFI");
        ((volatile uint32_t *)PMSG_BASE)[0] = 0;
        pmsg_put("STARBITE: fastboot entering UEFI at ");
        pmsg_hex(STAGE);
        pmsg_put(" size ");
        pmsg_hex(size);
        pmsg_put(" fdt ");
        pmsg_hex(fdt);
        pmsg_put("\n");
        zero_bytes(UEFI_LATCH_PAGE, UEFI_LATCH_END - UEFI_LATCH_PAGE);
        clean_bytes(UEFI_LATCH_PAGE, UEFI_LATCH_END);
        star_rwd1_arm();
        star_fb_restore((uint32_t)log[77]);
        return ram_handoff(log, 0x200, STAGE, fdt);
    }
    if (r->method == M_LINUX) {
        uint64_t dtb_size = r->dtb_size;
        if (!dtb_size)
            return fail(log, -73, "NO DTB: FASTBOOT --DTB X --HEADER-VERSION 2");
        if (dtb_size > (2UL << 20))
            return fail(log, -73, "LINUX DTB TOO LARGE");
        if (r->cmdline && r->cmdline[0]) {
            dtb_size = fdt_copy_append_bootargs(r->dtb, LINUX_DTB, (uint32_t)(UFS_DMA - LINUX_DTB), r->cmdline);
            if (!dtb_size)
                return fail(log, -74, "LINUX DTB HAS NO BOOTARGS");
        } else {
            copy_bytes(LINUX_DTB, r->dtb, dtb_size);
        }
        uint64_t now;
        if (em_rtc_epoch(&now))
            (void)fdt_chosen_set_u64(LINUX_DTB, "starbite,rtc-epoch", now);
        if (r->ramdisk_size) {
            if (r->ramdisk_size > EDIT_FDT - OS_RD)
                return fail(log, -75, "INITRD TOO LARGE");
            copy_bytes(OS_RD, r->ramdisk, r->ramdisk_size);
            if (!fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-start", OS_RD) ||
                !fdt_chosen_set_u64(LINUX_DTB, "linux,initrd-end", OS_RD + r->ramdisk_size))
                return fail(log, -75, "LINUX DTB HAS NO INITRD PROPERTIES");
        }
        uint8_t l31 = 0, l2 = 0;
        int rails = hw_sd_rails(&l31, &l2);
        star_fb_text(3, rails > 0 ? "MICROSD POWER ON" : rails == 0 ? "MICROSD POWER REFUSED" : "MICROSD POWER: NO ACPM");
        log[18] = (uint64_t)l31 << 8 | l2;
        dst = ram_kernel(log, r, OS_RD, -76, &span);
        if (!dst)
            return 0;
        clean_bytes(dst, dst + span);
        clean_bytes(LINUX_DTB, LINUX_DTB + dtb_size);
        return ram_handoff(log, 0x400, dst, LINUX_DTB);
    }
    if (r->method == M_ANDROID) {
        if (!r->ramdisk_size)
            return fail(log, -77, "ANDROID NEEDS A RAMDISK");
        if (r->ramdisk_size > EDIT_FDT - OS_RD)
            return fail(log, -77, "ANDROID RAMDISK TOO LARGE");
        copy_bytes(OS_RD, r->ramdisk, r->ramdisk_size);
        dst = ram_kernel(log, r, OS_RD, -78, &span);
        if (!dst)
            return 0;
        uint32_t total = __builtin_bswap32(*(volatile uint32_t *)(fdt + 4));
        uint32_t fsize = r->cmdline && r->cmdline[0] ?
            fdt_copy_append_bootargs(fdt, EDIT_FDT, LINUX_DTB - EDIT_FDT, r->cmdline) : 0;
        if (!fsize) {
            if ((r->cmdline && r->cmdline[0]) || total > LINUX_DTB - EDIT_FDT)
                return fail(log, -79, "ANDROID FDT FAILED");
            copy_bytes(EDIT_FDT, fdt, total);
            fsize = total;
        }
        if (!fdt_chosen_set_u64(EDIT_FDT, "linux,initrd-start", OS_RD) ||
            !fdt_chosen_set_u64(EDIT_FDT, "linux,initrd-end", OS_RD + r->ramdisk_size))
            return fail(log, -79, "NO INITRD PROPERTIES IN FDT");
        clean_bytes(dst, dst + span);
        clean_bytes(OS_RD, OS_RD + r->ramdisk_size);
        clean_bytes(EDIT_FDT, EDIT_FDT + fsize);
        star_fb_restore((uint32_t)log[77]);
        return ram_handoff(log, 0x500, dst, EDIT_FDT);
    }
    if (r->method == M_BARE) {
        dst = ram_kernel(log, r, OS_RD, -80, &span);
        if (!dst)
            return 0;
        clean_bytes(dst, dst + span);
        return ram_handoff(log, 0x600, dst, fdt);
    }
    return fail(log, -70, "UNKNOWN BOOT METHOD");
}

uintptr_t star_boot_ram(const struct star_ram *r, uintptr_t fdt, const char **why)
{
    /* a launch record too (no watchdog, no log fragments): `fastboot boot` images crash as well */
    launch_set("fastboot", "fastboot boot", r->method, CLF_FASTBOOT);
    uintptr_t e = ram_start((volatile uint64_t *)LOG_BASE, r, fdt);
    if (!e)
        launch_clear();
    *why = e ? 0 : boot_why;
    return e;
}
