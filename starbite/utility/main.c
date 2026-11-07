/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Utility OS 1 (docs/STARBITE-MASTER-PLAN.md 4.4, docs/UTILITY-OS.md): a pocket toolbox with no
 * Linux, on libsb. v1 (UO1 + UO2): the phone is a USB device with
 *   - a USB serial port: the Starbite shell (help, info, drives, log, menu, maint, off)
 *   - USB drives: a RAM disk (FAT32 "STARBITE", read-write, up to 1 GiB of DRAM bank 2) and the microSD
 *     card (read-only: it holds the Linux images and backups)
 * and a launcher (USB, Radio, Files, Measure, Everyday, Boot menu) in the menu's visual language.
 * Never writes UFS. Feeds the cluster-1 watchdog when the loader armed it (catalog wdt), so a hang comes
 * back to the loader's crash screen; with no USB host ever, no touch and no key for 10 minutes it restarts
 * into the menu (a countdown shows in the status bar for the last minute), so an unattended start always
 * comes back. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "sd.h"
#include "usb_dwc3.h"
#include "usb_phy.h"
#include "usb_desc.h"
#include "max77705_muic.h"
#include "msc.h"
#include "fat.h"

#define UT_VERSION "1 (UO1 + UO2)"

#define BANK2        0x880000000ULL
#define RAMDISK_MAX  (1ULL << 30)
#define ACM_RX       (USB_DMA_BASE + 0x9000UL)        /* 512 */
#define ACM_TX       (USB_DMA_BASE + 0xa000UL)        /* 1024 */
#define P_ACM_IN     USB_PHYS(USB_EP_ACM_IN)
#define P_ACM_OUT    USB_PHYS(USB_EP_ACM_OUT)

/* ---- watchdog (cluster 1, armed by the loader when the catalog entry has wdt) ------------------ */
#define WDT1 0x10060000UL
static uint64_t wdt_next;
static unsigned wdt_feeds;

static void wdt_feed(void)
{
    uint64_t now = sb_ms();
    if (now < wdt_next)
        return;
    wdt_next = now + 1000;
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (w[0] & (1U << 5)) {                           /* WTCON enable: WTCNT = WTDAT */
        w[2] = w[1];
        ++wdt_feeds;
    }
}

/* ---- drives ----------------------------------------------------------------------------------- */
static struct msc_lun lun[2];
static unsigned nlun;
static uint64_t ram_base, ram_bytes;
static const char *ram_why = "";
static int card_state;                                /* 0 not tried, 1 up, -1 failed */
static uint8_t *card_buf;                             /* cacheable staging for sd_read */

static int ram_read(struct msc_lun *l, uint64_t lba, uint32_t n, uint8_t *dst)
{
    const volatile uint64_t *s = (const volatile uint64_t *)(ram_base + lba * MSC_BLOCK);
    volatile uint64_t *d = (volatile uint64_t *)dst;
    (void)l;
    for (uint64_t i = 0; i < (uint64_t)n * MSC_BLOCK / 8; ++i)
        d[i] = s[i];
    return 0;
}

static int ram_write(struct msc_lun *l, uint64_t lba, uint32_t n, const uint8_t *src)
{
    volatile uint64_t *d = (volatile uint64_t *)(ram_base + lba * MSC_BLOCK);
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    (void)l;
    for (uint64_t i = 0; i < (uint64_t)n * MSC_BLOCK / 8; ++i)
        d[i] = s[i];
    return 0;
}

static int card_read(struct msc_lun *l, uint64_t lba, uint32_t n, uint8_t *dst)
{
    (void)l;
    while (n) {
        uint32_t k = n > 256 ? 256 : n;               /* 128 KiB staging */
        if (sd_read(lba, k, (uintptr_t)card_buf))
            return -1;
        volatile uint8_t *d = dst;
        for (uint32_t i = 0; i < k * MSC_BLOCK; ++i)
            d[i] = card_buf[i];
        dst += k * MSC_BLOCK;
        lba += k;
        n -= k;
    }
    return 0;
}

static int no_write(struct msc_lun *l, uint64_t lba, uint32_t n, const uint8_t *src)
{
    (void)l; (void)lba; (void)n; (void)src;
    return -1;
}

static void name(struct msc_lun *l, const char *v, const char *p, const char *s)
{
    sb_strcpy(l->vendor, sizeof(l->vendor), v);
    sb_strcpy(l->product, sizeof(l->product), p);
    sb_strcpy(l->serial, sizeof(l->serial), s);
}

static void ramdisk_setup(void)
{
    uint64_t base[8], size[8];
    int n = fdt_memory_banks(sb_boot.fdt, base, size, 8);
    uint64_t avail = 0;
    for (int i = 0; i < n; ++i)
        if (base[i] <= BANK2 && base[i] + size[i] > BANK2)
            avail = base[i] + size[i] - BANK2;
    if (n <= 0)
        ram_why = "no memory banks in the FDT";
    else if (!avail)
        ram_why = "no DRAM bank at 0x880000000";
    ram_bytes = avail > RAMDISK_MAX ? RAMDISK_MAX : avail & ~((1ULL << 20) - 1);
    ram_base = BANK2;
    /* never over a reserved-memory region (S-Boot's or the DT's): end the disk below the first one */
    char rn[16][32];
    uint64_t rb[16], rs[16];
    int nr = fdt_reserved(sb_boot.fdt, rn, rb, rs, 16);
    for (int i = 0; i < nr; ++i)
        if (rb[i] + rs[i] > ram_base && rb[i] < ram_base + ram_bytes) {
            uint64_t end = rb[i] > ram_base ? rb[i] - ram_base : 0;
            ram_bytes = end & ~((1ULL << 20) - 1);
            sb_logf("utility: RAM disk ends before reserved ", rn[i]);
        }
    struct fat32_info fi;
    if (ram_bytes < (256ULL << 20) ||
        fat32_format((volatile uint8_t *)ram_base, ram_bytes / MSC_BLOCK, "STARBITE", (uint32_t)sb_ticks(), &fi)) {
        if (!ram_why[0])
            ram_why = "too small for FAT32";
        ram_bytes = 0;
        return;
    }
    struct msc_lun *l = &lun[nlun++];
    name(l, "Starbite", "RAM disk", "SBRAM001");
    l->blocks = ram_bytes / MSC_BLOCK;
    l->ro = 0;
    l->present = 1;
    l->read = ram_read;
    l->write = ram_write;
    sb_logx("utility: RAM disk bytes ", ram_bytes);
}

static void card_setup(void)
{
    sd_allow();
    card_state = sd_up() ? 1 : -1;
    uint64_t sectors = card_state > 0 ? sd_sectors_hint() : 0;
    if (card_state < 0 || !sectors) {
        if (card_state > 0)
            card_state = -1;
        sb_log("utility: no microSD drive (card not up or no MBR)");
        return;
    }
    card_buf = sb_alloc(256 * MSC_BLOCK);
    struct msc_lun *l = &lun[nlun++];
    name(l, "Starbite", "microSD (ro)", "SBSD0001");
    l->blocks = sectors;
    l->ro = 1;
    l->present = 1;
    l->read = card_read;
    l->write = no_write;
    sb_logx("utility: microSD read-only, sectors ", sectors);
}

/* ---- USB -------------------------------------------------------------------------------------- */
static char serial[33] = "STARBITE";
static const char *usb_why = "";
static int usb_up, usb_routed, usb_ever;
#define IDLE_EXIT_MS (10U * 60U * 1000U)               /* no host, no touch or key for 10 min: back to the menu */
static uint64_t t_muic;

static void usb_start(void)
{
    const char *why = 0;
    usbd_stop();
    if (usb_phy_start(&why) || usbd_start(serial, &why)) {
        usbd_stop();
        usb_why = why ? why : "?";
        usb_up = 0;
        sb_logf("utility: USB did not start: ", usb_why);
        return;
    }
    usb_up = 1;
    usb_why = "";
}

/* ---- the Starbite shell on the USB serial port ------------------------------------------------- */
static char sh_line[128];
static unsigned sh_n;
static char sh_out[8192];
static unsigned sh_out_n;
static int sh_banner;

static void sh_put(const char *s)
{
    while (s && *s && sh_out_n < sizeof(sh_out)) {
        if (*s == '\n' && sh_out_n + 1 < sizeof(sh_out))
            sh_out[sh_out_n++] = '\r';
        sh_out[sh_out_n++] = *s++;
    }
}

static void sh_dec(uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    sh_put(u.b);
}

static void sh_size(uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_size(&u, v);
    sh_put(u.b);
}

static void sh_drives(void)
{
    for (unsigned i = 0; i < nlun; ++i) {
        sh_put("  LUN ");
        sh_dec(i);
        sh_put(": ");
        sh_put(lun[i].product);
        sh_put(", ");
        sh_size(lun[i].blocks * MSC_BLOCK);
        sh_put(lun[i].ro ? ", read-only" : ", read-write");
        sh_put(", read ");
        sh_size(lun[i].rd_bytes);
        sh_put(", written ");
        sh_size(lun[i].wr_bytes);
        sh_put("\n");
    }
    if (!nlun)
        sh_put("  no drives\n");
    sh_put("  SCSI commands ");
    sh_dec(msc_stats.cmds);
    sh_put(", errors ");
    sh_dec(msc_stats.errors);
    sh_put(", resets ");
    sh_dec(msc_stats.resets);
    sh_put("\n");
}

static void sh_run(const char *c)
{
    if (!c[0]) {
    } else if (sb_streq(c, "help")) {
        sh_put("help     this list\ninfo     version, uptime, watchdog\ndrives   the USB drives\n"
               "log      the Utility OS log\nmenu     restart into the Starbite menu\n"
               "maint    restart into Starbite Maintenance\noff      power off\n");
    } else if (sb_streq(c, "info")) {
        sh_put("Starbite Utility OS " UT_VERSION "\nuptime ");
        sh_dec(sb_ms() / 1000);
        sh_put(" s, watchdog feeds ");
        sh_dec(wdt_feeds);
        sh_put(", RAM disk at 0x880000000 ");
        sh_size(ram_bytes);
        sh_put(ram_why[0] ? " (" : "");
        sh_put(ram_why);
        sh_put(ram_why[0] ? ")\n" : "\n");
    } else if (sb_streq(c, "drives")) {
        sh_drives();
    } else if (sb_streq(c, "log")) {
        unsigned n = sb_log_count();
        for (unsigned i = n > 40 ? n - 40 : 0; i < n; ++i) {
            sh_put(sb_log_line(i));
            sh_put("\n");
        }
    } else if (sb_streq(c, "menu")) {
        sh_put("restarting into the Starbite menu\n");
        sh_line[0] = 1;                               /* acted on once the text is sent */
    } else if (sb_streq(c, "maint")) {
        sh_put("restarting into Starbite Maintenance\n");
        sh_line[0] = 2;
    } else if (sb_streq(c, "off")) {
        sh_put("powering off\n");
        sh_line[0] = 3;
    } else {
        sh_put("unknown command (help lists them)\n");
    }
}

static int sh_pending_action;

static void sh_input(const volatile uint8_t *d, uint32_t n)
{
    char echo[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; ++i) {
        char ch = (char)d[i];
        if (ch == '\r' || ch == '\n') {
            sh_put("\n");
            sh_line[sh_n] = 0;
            sh_run(sh_line);
            if ((unsigned char)sh_line[0] <= 3 && sh_line[0])
                sh_pending_action = sh_line[0];
            sh_n = 0;
            sh_put("utility> ");
        } else if ((ch == 0x7f || ch == 0x08) && sh_n) {
            --sh_n;
            sh_put("\b \b");
        } else if (ch >= 0x20 && ch < 0x7f && sh_n + 1 < sizeof(sh_line)) {
            sh_line[sh_n++] = ch;
            echo[0] = ch;
            sh_put(echo);
        }
    }
}

static void sh_service(void)
{
    if (!usbd_configured())
        return;
    if (!usbd_busy(P_ACM_OUT))
        (void)usbd_queue(P_ACM_OUT, ACM_RX, 512);
    if (sh_out_n && !usbd_busy(P_ACM_IN)) {
        unsigned k = sh_out_n > 1024 ? 1024 : sh_out_n;
        volatile uint8_t *tx = (volatile uint8_t *)ACM_TX;
        for (unsigned i = 0; i < k; ++i)
            tx[i] = (uint8_t)sh_out[i];
        for (unsigned i = k; i < sh_out_n; ++i)
            sh_out[i - k] = sh_out[i];
        sh_out_n -= k;
        (void)usbd_queue(P_ACM_IN, ACM_TX, k);
    }
}

/* ---- the service loop (USB, shell, watchdog): every UI loop turn and every helper's -------------- */
static void act(int a)
{
    if (a == 1)
        sb_reboot(SB_INFORM_NORMAL);                  /* no default OS: the loader shows its menu */
    if (a == 2)
        sb_boot_next("maint");
    if (a == 3)
        sb_power_off();
}

static void idle_check(void)
{
    if (usb_ever || usbd_configured())
        return;
    uint64_t idle = ui_idle_ms();
    if (idle < IDLE_EXIT_MS)
        return;
    sb_log("utility: 10 minutes without a USB host, a touch or a key: back to the menu");
    sb_reboot(SB_INFORM_NORMAL);
}

static void service(void)
{
    struct usbd_ev e;
    wdt_feed();
    idle_check();
    if (!usb_up)
        return;
    while (usbd_poll(&e)) {
        if (e.type == USBD_EV_CONFIGURED && e.value == 1)
            usb_ever = 1;
        msc_event(&e);
        if (e.type == USBD_EV_DONE && e.ep == P_ACM_OUT)
            sh_input((const volatile uint8_t *)ACM_RX, e.value);
        if (e.type == USBD_EV_LINE && (e.value & 1) && !sh_banner) {
            sh_banner = 1;
            sh_put("\nStarbite Utility OS " UT_VERSION ": type help\nutility> ");
        }
        if (e.type == USBD_EV_LINE && !(e.value & 1))
            sh_banner = 0;
    }
    msc_service();
    sh_service();
    if (sh_pending_action && !sh_out_n && !usbd_busy(P_ACM_IN)) {
        sb_mdelay(50);
        act(sh_pending_action);
        sh_pending_action = 0;
    }
    uint64_t now = sb_ms();
    if (now - t_muic > 250) {
        t_muic = now;
        if (muic_poll() == 2 && !msc_busy())          /* a replug: PHY and core from the start */
            usb_start();
    }
}

/* ---- UI ------------------------------------------------------------------------------------- */
static const char *const tile_name[6] = { "USB", "Radio", "Files", "Measure", "Everyday", "Boot menu" };
static const char *const tile_sub[6] = {
    "Drives and shell", "Scans (v2)", "Card and UFS (UO4)", "Charger, ruler (v1)", "Clock, calculator", "Leave Utility",
};
static int focus;

static void status_bar(void)
{
    ui_fill(0, 0, UI_W, UI_STATUS_H, C_BG);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "UTILITY  ");
    ub_s(&u, !usb_up ? "USB OFF" : usbd_configured() ? "USB: DRIVES + SHELL" : usbd_connected() ? "USB: CONNECTING" : "USB: NO HOST");
    ui_text(F_STATUS, 60, 74, u.b, usbd_configured() ? C_GREEN : C_L2);
    ub_init(&u);
    uint64_t idle = ui_idle_ms();
    int warn = !usb_ever && !usbd_configured() && idle + 60000U >= IDLE_EXIT_MS;
    if (warn) {
        ub_s(&u, "MENU IN ");
        ub_dec(&u, (IDLE_EXIT_MS - (idle < IDLE_EXIT_MS ? idle : IDLE_EXIT_MS)) / 1000);
        ub_s(&u, " S: TOUCH TO STAY");
    } else {
        ub_dec(&u, sb_ms() / 60000);
        ub_s(&u, " min");
    }
    ui_text_r(F_STATUS, UI_W - 60, 74, u.b, warn ? C_ORANGE : C_L2);
    ui_dirty(0, 0, UI_W, UI_STATUS_H);
}

static void tile_rect(int i, int *x, int *y, int *w, int *h)
{
    *w = 620;
    *h = 520;
    *x = 70 + (i % 2) * (*w + 60);
    *y = 560 + (i / 2) * (*h + 60);
}

static void draw_home(void)
{
    ui_fill(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H, C_BG);
    ui_text(F_TITLE, 70, 330, "Utility", C_L1);
    ui_text(F_DETAIL, 72, 430, "Starbite Utility OS " UT_VERSION, C_L2);
    for (int i = 0; i < 6; ++i) {
        int x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        ui_round(x, y, w, h, 48, i == focus ? C_CARD2 : C_CARD);
        if (i == focus)
            ui_frame(x, y, w, h, 6, C_STAR);
        ui_text(F_TITLE_S, x + 50, y + 200, tile_name[i], i == 5 ? C_ORANGE : C_L1);
        ui_text(F_DETAIL, x + 52, y + 300, tile_sub[i], C_L2);
        if (i == 0)
            ui_text(F_SMALL, x + 52, y + 420, usbd_configured() ? "connected" : "waiting for a computer",
                    usbd_configured() ? C_GREEN : C_L3);
    }
    ui_text(F_SMALL, 70, UI_H - 120, "Volume: move   Power: open   tap a tile", C_L3);
    ui_dirty_all();
    status_bar();
}

static void kv(struct page *p, const char *k, const char *v, uint32_t c) { page_kv(p, k, v, c); }

static void usb_fill(struct page *p)
{
    struct ub u;
    page_hdr(p, "USB");
    kv(p, "State", !usb_up ? "off" : usbd_configured() ? "configured" : usbd_connected() ? "connecting" : "no host",
       usbd_configured() ? C_GREEN : C_L2);
    kv(p, "Speed", usbd_configured() ? (usbd_full_speed() ? "full speed (12 Mb/s)" : "high speed (480 Mb/s)") : "-", C_L1);
    kv(p, "Device", "1209:0001 Starbite Utility", C_L2);
    if (usb_why[0])
        kv(p, "Error", usb_why, C_RED);
    page_hdr(p, "Drives");
    for (unsigned i = 0; i < nlun; ++i) {
        ub_init(&u);
        ub_size(&u, lun[i].blocks * MSC_BLOCK);
        ub_s(&u, lun[i].ro ? ", read-only" : ", read-write");
        kv(p, lun[i].product, u.b, C_L1);
        ub_init(&u);
        ub_s(&u, "read ");
        ub_size(&u, lun[i].rd_bytes);
        ub_s(&u, ", written ");
        ub_size(&u, lun[i].wr_bytes);
        kv(p, "  traffic", u.b, C_L2);
    }
    if (!ram_bytes)
        kv(p, "RAM disk", ram_why, C_ORANGE);
    if (card_state < 0)
        kv(p, "microSD", "not available", C_L3);
    ub_init(&u);
    ub_dec(&u, msc_stats.cmds);
    ub_s(&u, " commands, ");
    ub_dec(&u, msc_stats.errors);
    ub_s(&u, " errors");
    kv(p, "SCSI", u.b, C_L2);
    page_hdr(p, "Shell");
    kv(p, "Serial port", sh_banner ? "open" : "closed", sh_banner ? C_GREEN : C_L2);
    page_note(p, "On a Mac: the STARBITE disk appears in Finder; the shell is /dev/cu.usbmodem*. "
                 "Eject the disk before leaving: the RAM disk is gone when the phone restarts.", C_L3);
}

static void info_fill(struct page *p, int tile)
{
    page_hdr(p, tile_name[tile]);
    if (tile == 1) {
        page_note(p, "Wi-Fi, Bluetooth LE and NFC scanners need bare-metal radio drivers (v2). Until then "
                     "this tab shows what other OSes recorded.", C_L2);
    } else if (tile == 2) {
        page_note(p, "File manager (UFS read-only, card exFAT read-write), hashing, disk imaging and the hex "
                     "viewer: milestone UO4.", C_L2);
    } else if (tile == 3) {
        page_note(p, "USB charger tester, screen ruler at the panel's real ppi, dead-pixel test and colour "
                     "fields: next.", C_L2);
    } else {
        page_note(p, "Stopwatch, timer, world clock, scientific and programmer calculator, flashlight: next.",
                  C_L2);
    }
}

static void page_screen(const char *title, int tile)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_header_draw(title, "Utility", "", 0);
    uint64_t next = 0;
    for (;;) {
        if (sb_ms() >= next) {
            page_begin(&p);
            if (tile == 0)
                usb_fill(&p);
            else
                info_fill(&p, tile);
            page_end(&p);
            page_redraw(&p);
            status_bar();
            ui_present();
            next = sb_ms() + 500;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        service();
        if (e.type == EV_BACK || e.type == EV_SWIPE_R || e.type == EV_HOME ||
            (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        if (!page_scroll_event(&p, &e)) {
            if (e.type == EV_UP)
                page_set_scroll(&p, p.scroll - 400);
            else if (e.type == EV_DOWN)
                page_set_scroll(&p, p.scroll + 400);
        }
        page_fling_step(&p);
    }
    page_free(&p);
}

static void open_tile(int i)
{
    if (i == 5) {
        if (ui_dialog("Leave Utility?", "The phone restarts into the Starbite menu.",
                      ram_bytes ? "The RAM disk and its files are gone after the restart." : "",
                      "Restart", C_ORANGE))
            act(1);
        return;
    }
    page_screen(tile_name[i], i);
}

static void find_serial(void)
{
    char v[33];
    if (sb_bootarg("androidboot.serialno", v, sizeof(v)) > 0 && v[0])
        sb_strcpy(serial, sizeof(serial), v);
}

void sb_main(void)
{
    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    ui_tick = service;
    sb_log("Starbite Utility OS " UT_VERSION);
    wdt_feed();
    if (!sb_touch_start())
        sb_log("utility: no touch");
    find_serial();
    ramdisk_setup();
    wdt_feed();
    card_setup();
    msc_setup(lun, nlun);
    usb_routed = muic_route_usb();
    if (!usb_routed)
        sb_log("utility: MAX77705 did not answer (D+/D- route)");
    usb_start();
    ui_input_reset();
    draw_home();
    ui_present();
    uint64_t next = sb_ms() + 1000;
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 0);
        service();
        int open = -1;
        if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = (focus + (e.type == EV_UP ? 5 : 1)) % 6;
            draw_home();
        } else if (e.type == EV_SELECT) {
            open = focus;
        } else if (e.type == EV_TAP) {
            for (int i = 0; i < 6; ++i) {
                int x, y, w, h;
                tile_rect(i, &x, &y, &w, &h);
                if (e.x >= x && e.x < x + w && e.y >= y && e.y < y + h)
                    open = i;
            }
        }
        if (open >= 0) {
            focus = open;
            open_tile(open);
            ui_input_reset();
            draw_home();
        }
        if (sb_ms() >= next) {
            status_bar();
            next = sb_ms() + 1000;
        }
        ui_present();
    }
}
