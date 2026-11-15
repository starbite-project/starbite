/* SPDX-License-Identifier: GPL-2.0-only
 * Loader fastboot on the phone (fastboot.h "the loader side"): the USB loop around fastboot.c,
 * the platform functions it calls, and the loader-start check. docs/USB-FASTBOOT.md.
 *
 * Memory (docs/USB-FASTBOOT.md section 3), all in the loader's non-cacheable window (mmu.c):
 *   0x8c000000..0x91000000  download buffer = xnu_boot.c STAGE (ramboot.h); max-download-size
 *                           is that minus 64 KiB
 *   0x91000000..0x91050000  USB DMA (usb_dwc3.h USB_DMA_BASE): driver, then the class buffers
 *                           below; 0x91000000..0x91200000 is used by nothing else (STAGE ends at
 *                           0x91000000, the vendor log area starts at 0x91202000)
 */
#include "fastboot.h"
#include "usb_dwc3.h"
#include "usb_phy.h"
#include "max77705_muic.h"
#include "ramboot.h"
#include "catalog.h"
#include "fbtext.h"
#include "hw.h"
#include "../fdt.h"
#include "crash.h"
#include "sbfmt.h"
#include "recovery.h"          /* star_pmsg */
#include "sbfw.h"

_Static_assert(K_UP == 1U && K_DOWN == 2U && K_BIXBY == 4U && K_POWER == 8U, "sbfmt.c kq_key_bit");
_Static_assert(KQ_PRESS_MS > 40U && KQ_PRESS_MS < 300U, "a plain press for every menu (sbfmt.h)");

#define LOG_PAGE  0x91fff000UL
#define DL_MAX    (STAR_RAM_DL_END - STAR_RAM_DL - 0x10000UL)
#define DL_CHUNK  (8U << 20)

#define FB_RXBUF    (USB_DMA_BASE + 0x04000UL)   /* 512: a command */
#define ACM_RXBUF   (USB_DMA_BASE + 0x04400UL)   /* 512 */
#define FB_TXBUF    (USB_DMA_BASE + 0x08000UL)   /* 16 KiB: a response or upload data */
#define FB_TXCAP    0x4000U
#define ACM_TXBUF   (USB_DMA_BASE + 0x0c000UL)   /* 512 */
#define ACM_TXCAP   511U                         /* never a multiple of 512: no ZLP needed */
#define FB_SCRATCH  (USB_DMA_BASE + 0x10000UL)   /* 64 KiB each */
#define FB_STAGEBUF (USB_DMA_BASE + 0x20000UL)
#define CON_RING    (USB_DMA_BASE + 0x30000UL)
#define CON_SCRATCH (USB_DMA_BASE + 0x40000UL)
#define TEXT_CAP    0x10000U
_Static_assert(CON_SCRATCH + TEXT_CAP <= USB_DMA_BASE + USB_DMA_SIZE, "USB DMA window");

#define P_FB_IN   USB_PHYS(USB_EP_FB_IN)
#define P_FB_OUT  USB_PHYS(USB_EP_FB_OUT)
#define P_ACM_IN  USB_PHYS(USB_EP_ACM_IN)
#define P_ACM_OUT USB_PHYS(USB_EP_ACM_OUT)

#define NEXT_MAILBOX 0xFED17FE0UL                 /* menu.c: u64 "SBNEXT2!" + char key[24] */
#define NEXT2_MAGIC  0x21325458454e4253ULL
#define PMSG_RING    0xFED14000UL
#define DMESG_RING   0xFED10000UL

static struct {
    uintptr_t fdt, entry;
    char serial[33];
    int rx_data;              /* the fastboot OUT transfer in flight is download data */
    uint32_t rx_len;
    int cfg, dtr, zlp;
    uint32_t ncmd, nbytes, restarts;
    char status[40];
} fd;

/* ---- screen --------------------------------------------------------------------------------- */

static void upper(char *dst, const char *s, unsigned cap)
{
    unsigned i = 0;
    for (; s && s[i] && i + 1 < cap && i < STAR_FB_COLS; ++i)
        dst[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    dst[i] = 0;
}

/* the menus' background fastboot (the end of this file); it never draws on their screen */
static struct {
    int inited, inpoll, boot;
    unsigned mode;                  /* FBBG_* of the loop serving it now */
    uint64_t t_muic;
} bg;
static struct bgl bl;               /* wanted, up, retries: sbfmt.c bgl_* */
static struct kq keyq;

static void row(unsigned r, const char *s)
{
    char b[STAR_FB_COLS + 1];
    if (bl.up)
        return;
    upper(b, s, sizeof(b));
    star_fb_text(r, b);
}

void fbp_status(const char *line)
{
    row(4, line);
}

/* ---- platform ------------------------------------------------------------------------------- */

const char *fbp_serial(void)
{
    return fd.serial;
}

int fbp_battery(unsigned *mv, unsigned *pct)
{
    return muic_battery(mv, pct);
}

unsigned fbp_cat_count(void)
{
    return cat_count();
}

int fbp_cat_entry(unsigned i, const char **key, const char **name, unsigned *method, int *ready)
{
    const struct cat_ent *e = cat_get(i);
    if (!e)
        return 0;
    *key = e->key;
    *name = e->name;
    *method = e->method;
    *ready = star_entry_ready(i, 0);
    return 1;
}

int fbp_cat_find(const char *key)
{
    return cat_find(key);
}

void fbp_set_next(const char *key)
{
    volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
    volatile char *k = (volatile char *)(NEXT_MAILBOX + 8);
    for (unsigned i = 0; i < 4; ++i)
        mb[i] = 0;
    for (unsigned i = 0; i < 23 && key[i]; ++i)
        k[i] = key[i];
    __asm__ volatile("dsb sy" : : : "memory");
    mb[0] = NEXT2_MAGIC;
    __asm__ volatile("dsb sy" : : : "memory");
}

void fbp_clear_next(void)
{
    volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
    for (unsigned i = 0; i < 4; ++i)
        mb[i] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
}

int fbp_key(unsigned bits, unsigned ms)
{
    if (!bl.up || !bg.mode)
        return 0;
    return kq_push(&keyq, bits, ms);
}

const char *fbp_mode(void)
{
    if (!bl.up)
        return "fastboot";
    return bg.mode == FBBG_RECOVERY ? "recovery" : bg.mode == FBBG_CRASH ? "crash" : "menu";
}

/* `oem screen`: a copy of the panel at the top of the download buffer (so the menu's next frames
 * do not tear it), or the live panel when a download already reaches there */
#define SCREEN_BYTES ((uint32_t)STAR_FB_WIDTH * STAR_FB_HEIGHT * 4U)
#define SCREEN_COPY  (STAR_RAM_DL_END - 0x01100000UL)
_Static_assert(SCREEN_COPY + SCREEN_BYTES <= STAR_RAM_DL_END && SCREEN_COPY > STAR_RAM_DL, "screen copy");

int fbp_screen(uintptr_t *at, uint32_t *len)
{
    *len = SCREEN_BYTES;
    if (fb_dl_size() > SCREEN_COPY - STAR_RAM_DL) {
        *at = STAR_FB_BASE;
        return 1;
    }
    volatile const uint64_t *s = (volatile const uint64_t *)STAR_FB_BASE;
    volatile uint64_t *d = (volatile uint64_t *)SCREEN_COPY;
    for (uint32_t i = 0; i < SCREEN_BYTES / 8U; i += 4) {
        uint64_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
        d[i] = a;
        d[i + 1] = b;
        d[i + 2] = c;
        d[i + 3] = e;
    }
    __asm__ volatile("dsb sy" : : : "memory");
    *at = SCREEN_COPY;
    return 1;
}

const char *fbp_speed(void)
{
    if (!usbd_connected())
        return "none";
    return usbd_full_speed() ? "full" : "high";
}

int fbp_boot_prepare(unsigned method, const struct fb_image *img, const char **why)
{
    struct star_ram r;
    r.method = method;
    r.kernel = img->kernel;
    r.kernel_size = img->kernel_size;
    r.ramdisk = img->ramdisk;
    r.ramdisk_size = img->ramdisk_size;
    r.dtb = img->dtb;
    r.dtb_size = img->dtb_size;
    r.cmdline = img->cmdline;
    fd.entry = star_boot_ram(&r, fd.fdt, why);
    return fd.entry != 0;
}

int fbp_maint(unsigned variant, const char **why)
{
    fd.entry = star_maint_ram(variant, why);
    return fd.entry != 0;
}

/* A ramoops ring (persistent_ram_buffer: u32 "DBGC", start, size, then data) as text. The
 * loader caps its pmsg writes at 0x3F00 bytes (xnu_boot.c PMSG_DATA), Linux and XNU use the
 * whole 16 KiB: a full ring says which. */
static void ring_text(struct tb *t, uintptr_t base, const char *name)
{
    const volatile uint32_t *h = (const volatile uint32_t *)base;
    const volatile char *data = (const volatile char *)(base + 12);
    uint32_t cap = 16384U - 12U, start = h[1], size = h[2];
    tb_s(t, name);
    tb_c(t, ' ');
    tb_hex(t, base, 8);
    if (h[0] != 0x43474244U) {
        tb_s(t, ": no ring (no DBGC header)\n");
        return;
    }
    if (size > cap)
        size = cap;
    if (size == 0x3f00U)
        cap = 0x3f00U;
    if (start >= cap)
        start = 0;
    tb_s(t, ": ");
    tb_u(t, size);
    tb_s(t, " bytes\n");
    uint32_t at = (start + cap - size) % cap;
    for (uint32_t i = 0; i < size; ++i) {
        char c = data[(at + i) % cap];
        if (c)
            tb_c(t, c);
    }
    if (t->n && t->b[t->n - 1] != '\n')
        tb_c(t, '\n');
}

unsigned fbp_sbfw_table(uint8_t *out, unsigned cap)
{
    return sbfw_table_preview(out, cap);
}

unsigned fbp_log(const char *which, char *out, unsigned cap)
{
    struct tb t;
    tb_init(&t, out, cap);
    if (fb_streq(which, "pmsg")) {
        ring_text(&t, PMSG_RING, "pmsg");
    } else if (fb_streq(which, "dmesg")) {
        ring_text(&t, DMESG_RING, "dmesg");
    } else if (fb_streq(which, "loader")) {
        const volatile uint64_t *log = (const volatile uint64_t *)LOG_PAGE;
        tb_s(&t, "loader log page 0x91fff000 (nonzero words):\n");
        for (unsigned i = 0; i < 128; ++i) {
            uint64_t v = log[i];
            if (!v)
                continue;
            tb_s(&t, "log[");
            tb_u(&t, i);
            tb_s(&t, "] ");
            tb_hex(&t, v, 16);
            tb_c(&t, '\n');
        }
    } else if (fb_streq(which, "crash")) {
        return crash_text(out, cap);                    /* every saved report, as text */
    } else if (fb_streq(which, "crashraw")) {
        return crash_raw(out, cap);                     /* the 64 KiB crash log entry */
    } else if (fb_streq(which, "usb")) {
        tb_s(&t, "fastboot: commands ");
        tb_u(&t, fd.ncmd);
        tb_s(&t, ", bytes received ");
        tb_u(&t, fd.nbytes);
        tb_s(&t, ", link restarts (replug) ");
        tb_u(&t, fd.restarts);
        tb_s(&t, ", serial ");
        tb_s(&t, fd.serial);
        tb_s(&t, "\nbackground (menus): ");
        tb_s(&t, bl.wanted ? (bl.up ? "up" : bl.failed ? "failed, retried later" : "down") : "off");
        tb_s(&t, ", starts ");
        tb_u(&t, bl.starts);
        tb_s(&t, ", restarts ");
        tb_u(&t, bl.relinks);
        tb_s(&t, ", failures ");
        tb_u(&t, bl.fails);
        tb_c(&t, '\n');
        muic_diag(&t);
        usb_phy_diag(&t);
        usbd_diag(&t);
    } else {
        return 0;
    }
    return t.n;
}

/* androidboot.serialno from S-Boot's bootargs (what Android's adb shows), else "STARBITE" */
static void find_serial(void)
{
    uintptr_t v;
    uint32_t len;
    const char *key = "androidboot.serialno=";
    unsigned kl = fb_strlen(key), o = 0;
    fd.serial[0] = 0;
    if (fd.fdt && !(fd.fdt & 7) && fdt_chosen_raw(fd.fdt, "bootargs", &v, &len)) {
        const volatile char *a = (const volatile char *)v;
        for (uint32_t i = 0; i + kl < len; ++i) {
            if ((i && a[i - 1] != ' ') || a[i] != key[0])
                continue;
            unsigned m = 0;
            while (m < kl && a[i + m] == key[m])
                ++m;
            if (m < kl)
                continue;
            for (uint32_t j = i + kl; j < len && a[j] && a[j] != ' ' && o + 1 < sizeof(fd.serial); ++j)
                fd.serial[o++] = a[j];
            break;
        }
    }
    fd.serial[o] = 0;
    if (!o) {
        const char *s = "STARBITE";
        for (; s[o]; ++o)
            fd.serial[o] = s[o];
        fd.serial[o] = 0;
    }
}

/* ---- the loop ------------------------------------------------------------------------------- */

static void link_row(void)
{
    if (fd.cfg)
        row(2, usbd_full_speed() ? "USB: CONNECTED (FULL SPEED)" : "USB: CONNECTED (HIGH SPEED)");
    else if (usbd_connected())
        row(2, "USB: ENUMERATING");
    else
        row(2, "USB: WAITING FOR THE HOST");
}

static void on_event(const struct usbd_ev *e)
{
    if (e->type == USBD_EV_CONFIGURED) {
        fd.cfg = e->value == 1;
        fb_usb_reset();
        fd.rx_data = fd.zlp = 0;
        link_row();
    } else if (e->type == USBD_EV_RESET || e->type == USBD_EV_DISCONNECT) {
        fd.cfg = 0;
        fd.dtr = 0;
        fb_usb_reset();
        fd.rx_data = fd.zlp = 0;
        link_row();
    } else if (e->type == USBD_EV_CONNECT) {
        link_row();
    } else if (e->type == USBD_EV_LINE) {
        int dtr = e->value & 1U;
        if (dtr && !fd.dtr)
            con_hello();
        fd.dtr = dtr;
    } else if (e->type == USBD_EV_DONE) {
        if (e->ep == P_FB_OUT) {
            fd.nbytes += e->value;
            if (fd.rx_data) {
                fd.rx_data = 0;
                fb_rx_data(e->value);
            } else {
                uint8_t c[FB_CMD_MAX];
                const volatile uint8_t *s = (const volatile uint8_t *)FB_RXBUF;
                unsigned n = e->value < FB_CMD_MAX ? e->value : FB_CMD_MAX;
                for (unsigned i = 0; i < n; ++i)
                    c[i] = s[i];
                ++fd.ncmd;
                fb_command(c, n);
            }
        } else if (e->ep == P_ACM_OUT) {
            uint8_t c[512];
            const volatile uint8_t *s = (const volatile uint8_t *)ACM_RXBUF;
            unsigned n = e->value < sizeof(c) ? e->value : sizeof(c);
            for (unsigned i = 0; i < n; ++i)
                c[i] = s[i];
            con_input(c, n);
        }
    }
}

/* keep the endpoints fed */
static void pump(void)
{
    if (!fd.cfg)
        return;
    if (!usbd_busy(P_FB_IN)) {
        if (fd.zlp) {
            /* a 64-byte response at full speed fills a packet: end the transfer */
            if (!usbd_queue(P_FB_IN, FB_TXBUF, 0))
                fd.zlp = 0;
        } else {
            uint32_t n = fb_tx_next((uint8_t *)FB_TXBUF, FB_TXCAP);
            if (n && !usbd_queue(P_FB_IN, FB_TXBUF, n))
                fd.zlp = usbd_full_speed() && n == FB_RESP_MAX && fb_tx_was_msg();
        }
    }
    if (!usbd_busy(P_FB_OUT) && fb_tx_idle() && !usbd_busy(P_FB_IN) && !fd.zlp) {
        uint32_t want = fb_rx_want(), mps = usbd_full_speed() ? 64U : 512U;
        if (want) {
            uint32_t n = want < DL_CHUNK ? want : DL_CHUNK;
            n = (n + mps - 1) / mps * mps;
            if (!usbd_queue(P_FB_OUT, fb_rx_ptr(), n))
                fd.rx_data = 1;
        } else {
            (void)usbd_queue(P_FB_OUT, FB_RXBUF, FB_CMD_MAX);
        }
    }
    if (!usbd_busy(P_ACM_IN) && con_pending()) {
        unsigned n = con_tx_take((uint8_t *)ACM_TXBUF, ACM_TXCAP);
        if (n)
            (void)usbd_queue(P_ACM_IN, ACM_TXBUF, n);
    }
    if (!usbd_busy(P_ACM_OUT))
        (void)usbd_queue(P_ACM_OUT, ACM_RXBUF, 512);
}

static void service(void)
{
    struct usbd_ev e;
    while (usbd_poll(&e))
        on_event(&e);
    pump();
}

/* the console's last words reach the host before the cable goes quiet */
static void drain_console(void)
{
    uint64_t t0 = hw_ticks(), lim = hw_freq() / 4;
    while ((con_pending() || usbd_busy(P_ACM_IN)) && hw_ticks() - t0 < lim)
        service();
}

static int leave(int how)
{
    usbd_stop();
    row(2, how == FASTBOOT_BOOT ? "USB: STOPPED, STARTING" : "USB: STOPPED");
    return how;
}

/* everything queued (responses, console) out, at most ms */
static void flush(unsigned ms)
{
    uint64_t t0 = hw_ticks(), lim = hw_freq() / 1000U * ms;
    while ((!fb_tx_idle() || usbd_busy(P_FB_IN) || fd.zlp || con_pending() || usbd_busy(P_ACM_IN)) &&
           usbd_configured() && hw_ticks() - t0 < lim)
        service();
}

/* A replug: nothing in the controller notices (VBUS is forced valid in the PHY), and an earlier port saw a
 * link left as it was take the host's resets but never a SETUP again. So, as Android does on
 * every attach: the PHY and the core from the start. 0, or -1 with *why. */
static int relink(const char **why)
{
    usbd_stop();
    fb_usb_reset();
    fd.cfg = fd.dtr = fd.rx_data = fd.zlp = 0;
    ++fd.restarts;
    return usb_phy_start(why) || usbd_start(fd.serial, why) ? -1 : 0;
}

int fastboot_run(void)
{
    const char *why = 0;
    fbbg_stop();                                       /* the menus' link, if up: this one is its own */
    fd.fdt = ((volatile uint64_t *)LOG_PAGE)[3];      /* x19 from S-Boot (xnu_boot.S) */
    fd.entry = 0;
    fd.cfg = fd.dtr = fd.rx_data = fd.zlp = 0;
    fd.ncmd = fd.nbytes = fd.restarts = 0;
    find_serial();
    star_fb_mute(0);
    row(0, "STARBITE FASTBOOT");
    row(1, "BIXBY OR POWER: BACK TO THE MENU");
    row(2, "USB: STARTING");
    row(3, "");
    row(4, "");
    row(5, "");
    int routed = muic_route_usb();
    row(3, routed ? "D+/D-: ROUTED TO THE AP" : "D+/D-: MAX77705 DID NOT ANSWER");
    if (usb_phy_start(&why) || usbd_start(fd.serial, &why)) {
        row(2, "USB FAILED:");
        row(4, why);
        usbd_stop();
        uint64_t t0 = hw_ticks();
        while (hw_ticks() - t0 < 10 * hw_freq() && !(hw_keys() & (K_BIXBY | K_POWER)))
            ;
        return FASTBOOT_MENU;
    }
    fb_init(STAR_RAM_DL, (uint32_t)DL_MAX, (char *)FB_SCRATCH, TEXT_CAP, (char *)FB_STAGEBUF, TEXT_CAP);
    con_init((char *)CON_RING, TEXT_CAP, (char *)CON_SCRATCH, TEXT_CAP);
    link_row();

    while (hw_keys())                                  /* let go of Volume Up first */
        service();
    uint64_t t_muic = hw_ticks(), t_row = hw_ticks();
    char last_row[48];
    last_row[0] = 0;
    for (;;) {
        service();
        uint32_t v;
        int a = FB_ACT_NONE;
        if (!usbd_busy(P_FB_IN) && !fd.zlp)
            a = fb_action(&v);
        if (a == FB_ACT_NONE) {
            a = con_action(&v);
            if (a != FB_ACT_NONE)
                drain_console();
        }
        if (a == FB_ACT_MENU)
            return leave(FASTBOOT_MENU);
        if (a == FB_ACT_BOOT)
            return leave(FASTBOOT_BOOT);
        if (a == FB_ACT_REBOOT) {
            (void)leave(FASTBOOT_MENU);
            row(2, "REBOOTING");
            hw_reboot(v);
        }
        uint64_t now = hw_ticks();
        if (now - t_muic > hw_freq() / 2) {
            t_muic = now;
            /* A replug: nothing in the controller notices (VBUS is forced valid in the PHY), and
             * an earlier port saw a link left as it was take the host's resets but never a SETUP again. So,
             * as Android does on every attach: the PHY and the core from the start. */
            if (muic_poll() == 2) {
                if (relink(&why)) {
                    row(2, "USB RESTART FAILED:");
                    row(4, why);
                } else {
                    link_row();
                }
            }
        }
        if (now - t_row > hw_freq() / 4) {
            /* row 5: the download, else the link counters and line state (a photo of the
             * screen says how far enumeration got when the Mac shows nothing) */
            char b[48];
            struct tb t;
            t_row = now;
            tb_init(&t, b, sizeof(b));
            uint32_t want = fb_rx_want();
            if (want) {
                tb_s(&t, "DOWNLOAD: ");
                tb_u(&t, (want + (1U << 20) - 1) >> 20);
                tb_s(&t, " MB TO GO");
            } else {
                usbd_brief(&t);
                tb_s(&t, " ");
                tb_s(&t, usb_phy_line());
            }
            if (!fb_streq(b, last_row)) {
                row(5, b);
                for (unsigned i = 0; i < sizeof(last_row); ++i)
                    last_row[i] = i < t.n + 1 ? b[i] : 0;
            }
        }
        if (!fb_rx_want() && (hw_keys() & (K_BIXBY | K_POWER))) {
            while (hw_keys())
                service();
            return leave(FASTBOOT_MENU);
        }
    }
}

uintptr_t fastboot_entry(void)
{
    return fd.entry;
}

int fastboot_wanted(void)
{
    volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
    if (mb[0] == NEXT2_MAGIC) {
        const volatile char *k = (const volatile char *)(NEXT_MAILBOX + 8);
        const char *want = "fastboot";
        unsigned i = 0;
        while (want[i] && k[i] == want[i])
            ++i;
        if (!want[i] && !k[i]) {
            for (unsigned j = 0; j < 4; ++j)
                mb[j] = 0;
            __asm__ volatile("dsb sy" : : : "memory");
            return 1;
        }
    }
    /* Volume Up alone, held for 300 ms (S-Boot's own chords include Power or Bixby) */
    for (unsigned i = 0; i < 6; ++i) {
        if (hw_keys() != K_UP)
            return 0;
        hw_wait_ms(50);
    }
    return 1;
}

uintptr_t fastboot_hook(void)
{
    if (!fastboot_wanted())
        return 0;
    if (fastboot_run() != FASTBOOT_BOOT)
        return 0;
    return fastboot_entry();
}

/* ---- fastboot in the background while a menu waits for keys (fbbg.h) ------------------------- *
 * The same device, the same buffers and the same fastboot.c as fastboot_run(), served from the
 * menus' key loops instead of its own loop; nothing is drawn. One link for the whole boot: the
 * crash report, the boot menu and Starbite Recovery poll it in turn. When to start, serve or
 * restart it is sbfmt.c's bgl_* (host-tested): a link nobody served for BGL_GAP_MS (menu v7's
 * intro after the crash report, a POST run from recovery, Lab) is restarted before it is served,
 * since the host may have reset it meanwhile and given up on the port (health5 on the phone);
 * a failed start is retried with back-off. The download buffer (STAGE) and the USB area are free
 * while a menu runs: menu v7 lives at 0x83800000..0x88800000, the crash report at 0x9F000000..,
 * recovery's panel copy at 0x9C000000. */

static void pmsg2(const char *a, const char *b)
{
    star_pmsg(a);
    if (b)
        star_pmsg(b);
    star_pmsg("\n");
}

static uint64_t bg_ms(void)
{
    return hw_ticks() * 1000U / hw_freq();
}

/* Up from wherever it is: (fresh: the fastboot state from scratch, as at the first start of this
 * boot or after the loader stopped it) the MAX77705 route once a boot, PHY, core. 1 when up. */
static int bg_link(int fresh)
{
    const char *why = 0;
    if (fresh) {
        fd.fdt = ((volatile uint64_t *)LOG_PAGE)[3];
        fd.entry = 0;
        fd.ncmd = fd.nbytes = 0;
        find_serial();
    }
    int routed = 1;
    if (!bl.routed)
        routed = muic_route_usb();
    usbd_stop();                                       /* safe twice: a clean core either way */
    fb_usb_reset();
    fd.cfg = fd.dtr = fd.rx_data = fd.zlp = 0;
    if (usb_phy_start(&why) || usbd_start(fd.serial, &why)) {
        usbd_stop();
        pmsg2("STARBITE: usb: background fastboot could not start (retried later): ", why ? why : "?");
        return 0;
    }
    if (fresh) {
        fb_init(STAR_RAM_DL, (uint32_t)DL_MAX, (char *)FB_SCRATCH, TEXT_CAP, (char *)FB_STAGEBUF, TEXT_CAP);
        con_init((char *)CON_RING, TEXT_CAP, (char *)CON_SCRATCH, TEXT_CAP);
        kq_init(&keyq);
        pmsg2("STARBITE: usb: fastboot in the background while the menus wait",
              routed ? 0 : " (D+/D-: MAX77705 did not answer)");
    }
    bg.t_muic = hw_ticks();
    return 1;
}

void fbbg_enable(void)
{
    if (!bg.inited) {
        bgl_init(&bl);
        bg.inited = 1;
    }
    bgl_enable(&bl, 1);
}

int fbbg_pending(void)
{
    return bg.boot;
}

int fbbg_poll(unsigned where)
{
    if (bg.boot)
        return FBBG_BOOT;
    if (bg.inpoll || !bg.inited)
        return FBBG_NONE;
    unsigned what = bgl_poll(&bl, bg_ms(), bl.up && usbd_configured(), muic_vbus());
    if (what == BGL_IDLE)
        return FBBG_NONE;
    if (what != BGL_SERVE) {
        int ok = bg_link(what == BGL_START);
        bgl_result(&bl, what, ok, bg_ms());
        if (what == BGL_RELINK && bl.relinks <= 16U)
            pmsg2("STARBITE: usb: background link restarted (not served for a while, or no host took it)", 0);
        if (!ok)
            return FBBG_NONE;
    }
    bg.inpoll = 1;
    bg.mode = where;
    /* about 2 ms at most: an answer of many INFO lines goes out in bursts, downloads run in DMA */
    uint64_t t0 = hw_ticks(), lim = hw_freq() / 500U;
    for (;;) {
        service();
        if (fd.entry) {                                /* `fastboot boot` / `oem maint`: placed */
            bg.boot = 1;
            break;
        }
        uint32_t v = 0;
        int a = FB_ACT_NONE;
        if (!usbd_busy(P_FB_IN) && !fd.zlp)
            a = fb_action(&v);
        if (a == FB_ACT_NONE) {
            a = con_action(&v);
            if (a != FB_ACT_NONE)
                drain_console();
        }
        if (a == FB_ACT_REBOOT) {                      /* the key loops are safe points */
            flush(250);
            pmsg2("STARBITE: usb: reboot asked by the host", 0);
            usbd_stop();
            bgl_stopped(&bl);
            hw_reboot(v);
        }
        if (a == FB_ACT_BOOT) {
            bg.boot = 1;
            break;
        }
        /* FB_ACT_MENU (`fastboot continue`): already in a menu */
        if ((fb_tx_idle() && !usbd_busy(P_FB_IN) && !fd.zlp && !con_pending()) || hw_ticks() - t0 > lim)
            break;
    }
    uint64_t now = hw_ticks();
    if (!bg.boot && now - bg.t_muic > hw_freq() / 2) {
        bg.t_muic = now;
        if (muic_poll() == 2) {                        /* VBUS came back: a replug */
            ++fd.restarts;
            bgl_result(&bl, BGL_RELINK, bg_link(0), bg_ms());
        }
    }
    if (bl.up)
        bgl_served(&bl, bg_ms());
    bg.inpoll = 0;
    return bg.boot ? FBBG_BOOT : FBBG_NONE;
}

uintptr_t fbbg_entry(void)
{
    if (bl.up)
        flush(500);                                    /* the OKAY of `boot` / `oem maint` */
    uintptr_t e = bg.boot ? fd.entry : 0;
    fbbg_off();
    return e;
}

void fbbg_stop(void)
{
    if (bl.up) {
        flush(100);
        usbd_stop();
        bgl_stopped(&bl);
    }
    bg.mode = 0;
    kq_init(&keyq);
}

void fbbg_off(void)
{
    fbbg_stop();
    bgl_enable(&bl, 0);
    bg.boot = 0;
}

unsigned fbbg_inject(void)
{
    if (!bl.up || !keyq.n)
        return 0;
    return kq_bits(&keyq, bg_ms());
}
