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
#include "ut.h"
#include "hid.h"
#include "hash.h"
#include "ufs.h"

#define UT_VERSION "2.0"

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
#define MAX_LUN 10
static struct msc_lun lun[MAX_LUN];
static unsigned nlun, nlun_base;                      /* nlun_base: RAM disk + card; the rest: UFS (opt-in) */
static int ufs_offered;
/* UFS partitions offered as read-only drives (master plan 4.4: never-touch ones are not offered) */
static const char *const ufs_allow[] = { "BOOT", "SYSTEM", "VENDOR", "ODM", "WINESP", "WINDOWS", "A16DATA", "USERDATA" };

/* 1 for a partition the USB drives may offer (and Files may show the bytes of) */
int ut_ufs_allowed(const char *name)
{
    for (unsigned i = 0; i < sizeof(ufs_allow) / sizeof(ufs_allow[0]); ++i)
        if (sb_streq(name, ufs_allow[i]))
            return 1;
    return 0;
}
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
    /* the previous OS's memory must not reach a connected computer: zero what the format left (data clusters) */
    {
        uint64_t from = ((uint64_t)fi.data_sector + 8) * MSC_BLOCK;
        volatile uint64_t *q = (volatile uint64_t *)(ram_base + from);
        for (uint64_t i = 0, n = (ram_bytes - from) / 8; i < n; ++i) {
            q[i] = 0;
            if (!(i & 0x7fffffU))                     /* every 64 MiB */
                wdt_feed();
        }
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

/* a UFS partition as 512-byte blocks: reads cover whole 4 KiB blocks into a staging buffer */
static uint8_t *ufs_stage;                            /* MSC_CHUNK + 8 KiB, cacheable */

static int ufs_lun_read(struct msc_lun *l, uint64_t lba, uint32_t n, uint8_t *dst)
{
    uint64_t first = l->ctx;                          /* the partition's first 4 KiB block */
    while (n) {
        uint64_t byte = lba * MSC_BLOCK;
        uint32_t k = n > MSC_CHUNK / MSC_BLOCK ? MSC_CHUNK / MSC_BLOCK : n;
        uint64_t b0 = byte / 4096, b1 = (byte + (uint64_t)k * MSC_BLOCK + 4095) / 4096;
        if (ufs_read_to(first + b0, b1 - b0, (uintptr_t)ufs_stage))
            return -1;
        const uint8_t *src = ufs_stage + (byte - b0 * 4096);
        volatile uint8_t *d = dst;
        for (uint32_t i = 0; i < k * MSC_BLOCK; ++i)
            d[i] = src[i];
        dst += k * MSC_BLOCK;
        lba += k;
        n -= k;
    }
    return 0;
}

/* Offer (on = 1) or withdraw the allow-listed UFS partitions as read-only drives; USB reconnects so the host
 * reads the new LUN count. */
static void usb_start(void);
static void ufs_offer(int on)
{
    nlun = nlun_base;
    ufs_offered = 0;
    if (on && ufs_up() && gpt_load() > 0) {
        if (!ufs_stage)
            ufs_stage = sb_alloc(MSC_CHUNK + 8192);
        for (unsigned i = 0; i < sizeof(ufs_allow) / sizeof(ufs_allow[0]) && nlun < MAX_LUN && ufs_stage; ++i) {
            uint64_t f, e;
            if (!gpt_find(ufs_allow[i], &f, &e))
                continue;
            struct msc_lun *l = &lun[nlun++];
            for (unsigned k = 0; k < sizeof(*l); ++k)
                ((uint8_t *)l)[k] = 0;
            char sn[9] = "SBUFS0";                    /* a serial per partition: SBUFS001.. */
            sn[6] = (char)('0' + (i + 1) / 10);
            sn[7] = (char)('0' + (i + 1) % 10);
            sn[8] = 0;
            name(l, "Starbite", ufs_allow[i], sn);
            l->blocks = (e - f + 1) * (4096 / MSC_BLOCK);
            l->ro = 1;
            l->present = 1;
            l->read = ufs_lun_read;
            l->write = no_write;
            l->ctx = f;
        }
        ufs_offered = nlun > nlun_base;
    }
    msc_setup(lun, nlun);
    usb_start();
    sb_logx("utility: USB drives now ", nlun);
}

static char serial[33];                               /* defined with the USB code below */
static void ramdisk_files(void);

/* ---- files on the RAM disk: README, the phone, the previous boot's log (pmsg), this log -------------- */
#define PMSG_DATA 0x3F00U
static char pmsg_snap[PMSG_DATA + 64];
static unsigned pmsg_len;

/* the pmsg ring as it was when the Utility OS started: the loader's lines and the previous OS's */
static void pmsg_snapshot(void)
{
    const volatile uint32_t *h = (const volatile uint32_t *)SB_PMSG;
    const volatile uint8_t *d = (const volatile uint8_t *)(SB_PMSG + 12);
    pmsg_len = 0;
    if (h[0] != 0x43474244U || h[1] >= PMSG_DATA || h[2] > PMSG_DATA)
        return;
    uint32_t start = h[1], size = h[2];
    uint32_t from = size < PMSG_DATA ? (start + PMSG_DATA - size) % PMSG_DATA : start;
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t c = d[(from + i) % PMSG_DATA];
        pmsg_snap[pmsg_len++] = (char)(c == '\n' || (c >= 0x20 && c < 0x7f) ? c : '.');
    }
}

static char fbuf[16384];
static unsigned fn;
static void fs_(const char *s) { while (s && *s && fn + 1 < sizeof(fbuf)) fbuf[fn++] = *s++; }
static void fd_(uint64_t v) { struct ub u; ub_init(&u); ub_dec(&u, v); fs_(u.b); }
static void fz_(uint64_t v) { struct ub u; ub_init(&u); ub_size(&u, v); fs_(u.b); }
static void fh_(uint64_t v, unsigned d) { struct ub u; ub_init(&u); ub_hex(&u, v, d); fs_(u.b); }

static void ramdisk_files(void)
{
    struct sb_rtc r;
    sb_rtc_read(&r);
    if (r.ok)
        fat32_set_date(r.y, r.mo, r.d);
    fn = 0;
    fs_("Starbite Utility OS " UT_VERSION "\r\n\r\nThis drive is the phone's RAM: read-write, gone when the phone restarts.\r\n"
        "PHONE.TXT  the phone, its memory and its UFS partitions\r\nPMSG.TXT   the log of the previous boot (the "
        "loader and the OS before this one)\r\nLOG.TXT    this Utility OS's log at start\r\n\r\nThe shell is the "
        "USB serial port (on a Mac /dev/cu.usbmodem*): type help.\r\n");
    (void)fat32_add_file("README.TXT", (const uint8_t *)fbuf, fn);
    fn = 0;
    fs_("Starbite Utility OS " UT_VERSION "\r\nserial ");
    fs_(serial);
    fs_("\r\n\r\nmemory banks (FDT)\r\n");
    uint64_t base[8], size[8];
    int nb = fdt_memory_banks(sb_boot.fdt, base, size, 8);
    for (int i = 0; i < nb; ++i) {
        fs_("  ");
        fh_(base[i], 9);
        fs_("  ");
        fz_(size[i]);
        fs_("\r\n");
    }
    fs_("RAM disk at 0x880000000: ");
    fz_(ram_bytes);
    fs_("\r\nmicroSD: ");
    if (card_state > 0) {
        fz_(sd_sectors_hint() * 512ULL);
        fs_(" (read-only drive)");
    } else {
        fs_("not available");
    }
    fs_("\r\n\r\nUFS partitions (LU0 GPT, 4 KiB blocks)\r\n");
    if (ufs_up() && gpt_load() > 0) {
        for (int i = 0; i < gpt_n; ++i) {
            fs_("  ");
            fs_(gpt_name[i]);
            fs_("  first ");
            fd_(gpt_first[i]);
            fs_("  blocks ");
            fd_(gpt_last[i] - gpt_first[i] + 1);
            fs_("  ");
            fz_((gpt_last[i] - gpt_first[i] + 1) * 4096ULL);
            fs_("\r\n");
        }
    } else {
        fs_("  UFS not readable\r\n");
    }
    (void)fat32_add_file("PHONE.TXT", (const uint8_t *)fbuf, fn);
    if (pmsg_len)
        (void)fat32_add_file("PMSG.TXT", (const uint8_t *)pmsg_snap, pmsg_len);
    fn = 0;
    for (unsigned i = 0; i < sb_log_count(); ++i) {
        fs_(sb_log_line(i));
        fs_("\r\n");
    }
    (void)fat32_add_file("LOG.TXT", (const uint8_t *)fbuf, fn);
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
static int sh_hash_watch;                             /* the shell started the running hash job */
static uint64_t sh_num(const char *s);

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

static void sh_parts(void)
{
    if (!ufs_up() || gpt_load() <= 0) {
        sh_put("UFS: not readable\n");
        return;
    }
    for (int i = 0; i < gpt_n; ++i) {
        sh_put("  ");
        sh_put(gpt_name[i]);
        sh_put("  ");
        sh_dec((gpt_last[i] - gpt_first[i] + 1) * 4096ULL);
        sh_put(" bytes\n");
    }
}

static void sh_run(const char *c)
{
    if (!c[0]) {
    } else if (sb_streq(c, "help")) {
        sh_put("help     this list\ninfo     version, uptime, watchdog\ndrives   the USB drives\n"
               "ufs on|off  offer the UFS partitions as read-only drives (USB reconnects)\n"
               "parts    the UFS partitions (read only)\nhash NAME [BYTES]  SHA-256 + CRC32 of a partition, "
               "card, or their first BYTES\n"
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
        struct sb_rtc r;                              /* raw, as the RTC holds it (UTC unless Windows set it) */
        sb_rtc_read(&r);
        struct ub u;
        ub_init(&u);
        if (r.ok) {
            ub_s(&u, "rtc ");
            ub_dec(&u, r.y);
            ub_c(&u, '-');
            ub_dec2(&u, r.mo);
            ub_c(&u, '-');
            ub_dec2(&u, r.d);
            ub_c(&u, ' ');
            ub_dec2(&u, r.h);
            ub_c(&u, ':');
            ub_dec2(&u, r.mi);
            ub_c(&u, ':');
            ub_dec2(&u, r.s);
            ub_s(&u, " (raw)\n");
        } else {
            ub_s(&u, "rtc: no answer\n");
        }
        sh_put(u.b);
    } else if (sb_streq(c, "drives")) {
        sh_drives();
    } else if (sb_streq(c, "ufs on") || sb_streq(c, "ufs off")) {
        sh_put(c[5] == 'n' ? "offering the UFS partitions read-only: USB reconnects\n"
                           : "withdrawing the UFS partitions: USB reconnects\n");
        sh_line[0] = c[5] == 'n' ? 4 : 5;
    } else if (sb_streq(c, "parts")) {
        sh_parts();
    } else if (sb_prefix(c, "hash ")) {
        char name[24];
        unsigned k = 0;
        const char *a = c + 5;
        while (*a == ' ')
            ++a;
        while (*a && *a != ' ' && k + 1 < sizeof(name))
            name[k++] = *a++;
        name[k] = 0;
        while (*a == ' ')
            ++a;
        uint64_t bytes = sh_num(a);
        int r = sb_streq(name, "microSD") || sb_streq(name, "card") ? hash_start_card(bytes)
                                                                      : hash_start_part(name, bytes);
        if (r == 0) {
            sh_hash_watch = 1;
            sh_put("hashing (read only)... the result follows\n");
        } else {
            sh_put(r == -1 ? "a hash is already running\n" : r == -4 ? "no such partition (parts lists them)\n"
                                                                     : "cannot read it\n");
        }
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
static uint64_t sh_action_at;                         /* when the action runs at the latest */

static uint64_t sh_num(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static void sh_hash_report(void)
{
    if (!sh_hash_watch || hash_job.state == HASH_RUNNING)
        return;
    sh_hash_watch = 0;
    if (hash_job.state == HASH_DONE) {
        char hex[65];
        hash_hex(hex);
        sh_put(hex);
        sh_put("  ");
        sh_put(hash_job.name);
        sh_put(" (");
        sh_dec(hash_job.bytes);
        sh_put(" bytes, crc32 ");
        struct ub u;
        ub_init(&u);
        ub_hexn(&u, hash_job.crc, 8);
        sh_put(u.b);
        sh_put(", ");
        sh_dec(hash_job.t1 > hash_job.t0 ? hash_job.bytes / 1000 / (hash_job.t1 - hash_job.t0) : 0);
        sh_put(" MB/s)\nutility> ");
    } else {
        sh_put("hash: stopped (read error or cancelled)\nutility> ");
    }
}

static void sh_input(const volatile uint8_t *d, uint32_t n)
{
    char echo[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; ++i) {
        char ch = (char)d[i];
        if (ch == '\r' || ch == '\n') {
            sh_put("\n");
            sh_line[sh_n] = 0;
            sh_run(sh_line);
            if ((unsigned char)sh_line[0] <= 5 && sh_line[0]) {
                sh_pending_action = sh_line[0];
                sh_action_at = sb_ms() + 300;         /* even if the host stops reading the reply */
            }
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

/* a copy into the DMA window, not inlined: with the window's fixed address folded into the loop, the
 * compiler built a base pointer "array - 0xa700a000" whose relocation lies outside the image (check-image) */
static void __attribute__((noipa)) to_dma(uintptr_t dst, const char *src, unsigned n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    for (unsigned i = 0; i < n; ++i)
        d[i] = (uint8_t)src[i];
}

static void sh_service(void)
{
    if (!usbd_configured())
        return;
    if (!usbd_busy(P_ACM_OUT))
        (void)usbd_queue(P_ACM_OUT, ACM_RX, 512);
    if (sh_out_n && !usbd_busy(P_ACM_IN)) {
        unsigned k = sh_out_n > 511 ? 511 : sh_out_n;    /* never a multiple of 512 (high speed) */
        if (usbd_full_speed() && k > 1 && k % 64 == 0)
            --k;                                          /* nor of 64 (full speed): no zero-length packet */
        to_dma(ACM_TX, sh_out, k);
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
    if (a == 4 || a == 5)
        ufs_offer(a == 4);
}

int ut_awake;

static void idle_check(void)
{
    if (ut_awake || usb_ever || usbd_configured() || hash_job.state == HASH_RUNNING)
        return;
    uint64_t idle = ui_idle_ms();
    if (idle < IDLE_EXIT_MS)
        return;
    sb_log("utility: 10 minutes without a USB host, a touch or a key: back to the menu");
    sb_reboot(SB_INFORM_NORMAL);
}

/* ---- screen saver ---------------------------------------------------------------------------- */
/* After 2 minutes without a touch or key the panel goes black (OLED: those pixels are off, so nothing burns
 * in and the battery lasts) with a dim status that moves every 30 s. A touch or key brings the page back as
 * it was (the back buffer is kept aside) and goes no further. USB, the shell, hashing, the idle return and
 * the watchdog keep running: the saver's loop polls through ui_poll, whose tick is ut_service. */
#define SAVER_MS (2U * 60U * 1000U)
static int in_saver, saver_off;
unsigned ut_saver_wakes;                              /* pages that track a touch reset it when this moves */
static uint32_t *saver_buf;

static void saver_draw(unsigned k)
{
    int x = 60 + (int)((k * 397U) % 400U), y = 260 + (int)((k * 911U) % 2300U);
    struct ub u;
    ui_fill(0, 0, UI_W, UI_H, 0xff000000U);
    ub_init(&u);
    ub_s(&u, "Starbite Utility   ");
    ut_clock_status(&u);                              /* local time and battery */
    ui_text(F_SMALL, x, y, u.b, C_L3);
    ub_init(&u);
    if (hash_job.state == HASH_RUNNING) {
        ub_s(&u, "hashing ");
        ub_s(&u, hash_job.name);
        ub_s(&u, "  ");
        ub_dec(&u, hash_job.bytes ? hash_job.done * 100U / hash_job.bytes : 0);
        ub_s(&u, " %");
    } else if (usbd_configured()) {
        ub_s(&u, "USB: ");
        ub_dec(&u, nlun);
        ub_s(&u, nlun == 1 ? " drive + shell" : " drives + shell");
    } else if (!usb_ever) {
        uint64_t idle = ui_idle_ms();
        ub_s(&u, "no computer: back to the menu in ");
        ub_dec(&u, (IDLE_EXIT_MS - (idle < IDLE_EXIT_MS ? idle : IDLE_EXIT_MS)) / 1000U);
        ub_s(&u, " s");
    } else {
        ub_s(&u, "USB: unplugged");
    }
    ui_text(F_SMALL, x, y + 70, u.b, C_L3);
    ui_text(F_SMALL, x, y + 140, "touch or press a key", C_L3);
    ui_dirty_all();
    ui_present();
}

static void saver_check(void)
{
    if (in_saver || saver_off || ut_awake || ui_idle_ms() < SAVER_MS)
        return;
    uint64_t px = (uint64_t)UI_W * UI_H;
    if (!saver_buf && !(saver_buf = sb_alloc(px * 4U))) {
        saver_off = 1;                                /* no room in the arena: no saver */
        sb_log("utility: no memory for the screen saver");
        return;
    }
    in_saver = 1;
    uint32_t *bb = ui_back();
    for (uint64_t i = 0; i < px; ++i)
        saver_buf[i] = bb[i];
    struct ui_rect clip = ui_get_clip();
    ui_noclip();
    sb_log("utility: screen saver");
    unsigned k = 0;
    uint64_t next = 0, move = sb_ms() + 30000U;
    for (;;) {
        if (sb_ms() >= move) {
            ++k;
            move = sb_ms() + 30000U;
            next = 0;
        }
        if (sb_ms() >= next) {
            saver_draw(k);
            next = sb_ms() + 1000U;
        }
        struct ui_ev e;
        ui_poll(&e, 100);
        if (e.type != EV_NONE)
            break;
    }
    ui_input_reset();                                 /* the waking touch or key goes nowhere */
    ++ut_saver_wakes;
    for (uint64_t i = 0; i < px; ++i)
        bb[i] = saver_buf[i];
    ui_set_clip(clip);
    ui_dirty_all();
    ui_present();
    in_saver = 0;
}

void ut_service(void)
{
    struct usbd_ev e;
    wdt_feed();
    idle_check();
    saver_check();
    uint64_t now = sb_ms();
    if (now - t_muic >= 500) {                        /* max77705_muic.c's schedule */
        t_muic = now;
        /* a replug (VBUS is forced valid in the PHY, so the core never sees it: the PHY and the core from
         * the start, as the loader does) or a start that failed earlier */
        if (muic_poll() == 2 || !usb_up) {
            msc_setup(lun, nlun);
            hid_reset();
            usb_start();
        }
    }
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
    hid_service();
    hash_step();
    sh_hash_report();
    sh_service();
    if (sh_pending_action && ((!sh_out_n && !usbd_busy(P_ACM_IN)) || sb_ms() >= sh_action_at)) {
        int a = sh_pending_action;
        if ((a == 4 || a == 5) && msc_busy() && sb_ms() < sh_action_at + 2000) {
            ;                                         /* UFS drives: wait for the drive transfer, 2 s at most */
        } else {
            sh_pending_action = 0;
            sb_mdelay(50);
            act(a);
        }
    }
}


/* ---- UI ------------------------------------------------------------------------------------- */
#define NTILE 8
enum { T_USB, T_REMOTE, T_SENSORS, T_RADIO, T_FILES, T_MEASURE, T_EVERYDAY, T_LEAVE };
static const char *const tile_name[NTILE] = { "USB", "Remote", "Sensors", "Radio", "Files", "Measure", "Everyday",
                                              "Boot menu" };
static const char *const tile_sub[NTILE] = {
    "Drives and shell", "Keys, gamepad, media", "Battery, heat, touch", "Chips and plans", "Card, partitions, hashes",
    "Charger, ruler, pixels", "Clocks, calculator, QR", "Leave Utility",
};
static int focus;

void ut_status_bar(void)
{
    ui_fill(0, 0, UI_W, UI_STATUS_H, C_BG);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "UTILITY  ");
    ub_s(&u, !usb_up ? "USB OFF" : usbd_configured() ? "USB: DRIVES + SHELL" : usbd_connected() ? "USB: CONNECTING" : "USB: NO HOST");
    ui_text(F_STATUS, 60, 74, u.b, usbd_configured() ? C_GREEN : C_L2);
    ub_init(&u);
    uint64_t idle = ui_idle_ms();
    int warn = !ut_awake && hash_job.state != HASH_RUNNING && !usb_ever && !usbd_configured() &&
               idle + 60000U >= IDLE_EXIT_MS;            /* the same conditions as idle_check */
    if (warn) {
        ub_s(&u, "MENU IN ");
        ub_dec(&u, (IDLE_EXIT_MS - (idle < IDLE_EXIT_MS ? idle : IDLE_EXIT_MS)) / 1000);
        ub_s(&u, " S: TOUCH TO STAY");
    } else {
        ut_clock_status(&u);                          /* local time and battery (clock.c) */
    }
    ui_text_r(F_STATUS, UI_W - 60, 74, u.b, warn ? C_ORANGE : C_L2);
    ui_dirty(0, 0, UI_W, UI_STATUS_H);
}

static void tile_rect(int i, int *x, int *y, int *w, int *h)
{
    *w = 620;
    *h = 470;
    *x = 70 + (i % 2) * (*w + 60);
    *y = 520 + (i / 2) * (*h + 46);
}

static void draw_home(void)
{
    ui_fill(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H, C_BG);
    ui_text(F_TITLE, 70, 330, "Utility", C_L1);
    ui_text(F_DETAIL, 72, 430, "Starbite Utility OS " UT_VERSION, C_L2);
    for (int i = 0; i < NTILE; ++i) {
        int x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        if (i == focus)
            ui_round(x - 8, y - 8, w + 16, h + 16, 56, C_STAR);   /* rounded focus ring */
        ui_round(x, y, w, h, 48, i == focus ? C_CARD2 : C_CARD);
        ui_text(F_TITLE_S, x + 50, y + 180, tile_name[i], i == T_LEAVE ? C_ORANGE : C_L1);
        ui_text_cut(F_DETAIL, x + 52, y + 280, tile_sub[i], w - 80, C_L2);
        if (i == T_USB)
            ui_text(F_SMALL, x + 52, y + 390, usbd_configured() ? "connected" : "waiting for a computer",
                    usbd_configured() ? C_GREEN : C_L3);
    }
    ui_text(F_SMALL, 70, UI_H - 120, "Volume: move   Power: open   tap a tile", C_L3);
    ui_dirty_all();
    ut_status_bar();
}

static void kv(struct page *p, const char *k, const char *v, uint32_t c) { page_kv(p, k, v, c); }
static int row_pad = -1, row_ufs = -1;

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
    page_hdr(p, "Keyboard and mouse");
    ub_init(&u);
    ub_dec(&u, hid_stats.reports);
    ub_s(&u, " reports, ");
    ub_dec(&u, hid_stats.keys);
    ub_s(&u, " keys, ");
    ub_dec(&u, hid_stats.clicks);
    ub_s(&u, " clicks");
    kv(p, "HID", u.b, C_L2);
    row_pad = page_button(p, "Trackpad and keys", C_STAR);
    page_hdr(p, "UFS partitions (read only)");
    row_ufs = page_button(p, ufs_offered ? "Stop offering UFS partitions" : "Offer UFS partitions as drives",
                          ufs_offered ? C_ORANGE : C_STAR);
    page_note(p, "BOOT, SYSTEM, VENDOR, ODM, WINESP, WINDOWS, A16DATA, USERDATA, read only. USB reconnects. "
                 "EFS, PARAM and the like are never offered.", C_L3);
    page_note(p, "On a Mac: the STARBITE disk appears in Finder; the shell is /dev/cu.usbmodem*. "
                 "Eject the disk before leaving: the RAM disk is gone when the phone restarts.", C_L3);
}

static void info_fill(struct page *p, int tile)
{
    if (tile == T_RADIO) {
        page_hdr(p, "What the phone has");
        page_kv(p, "Wi-Fi + Bluetooth", "BCM4361 on PCIe and a UART", C_L1);
        page_kv(p, "NFC", "Samsung sec-nfc chip on I2C (0x27)", C_L1);
        page_kv(p, "GNSS + sensor hub", "BCM4775 on SPI", C_L1);
        page_kv(p, "Cellular", "Shannon modem", C_L1);
        page_hdr(p, "Coming");
        page_note(p, "Wi-Fi scan: a portable BCM4361 core is being written (its firmware loads from VENDOR); "
                     "then this tile lists the networks around, read only.", C_L2);
        page_note(p, "NFC tag reader: needs its power rail (LDO38), the clock and the GPIO expander's VEN pin "
                     "switched on with you present the first time; then tag type and UID, NDEF text and links. "
                     "Never payment cards.", C_L2);
        page_note(p, "Bluetooth LE scan after Wi-Fi (the same chip). The modem stays off: no transmitting.", C_L2);
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
            ut_status_bar();
            ui_present();
            next = sb_ms() + 500;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_SWIPE_R || e.type == EV_HOME ||
            (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        int pick = -1;
        if (!page_scroll_event(&p, &e)) {
            if (tile == 0 && row_pad >= 0 && (e.type == EV_UP || e.type == EV_DOWN)) {
                int f = p.focus == row_pad ? row_ufs : row_pad;
                page_set_focus(&p, f);
                page_keep_visible(&p, f);
                ui_present();
            } else if (e.type == EV_UP) {
                page_set_scroll(&p, p.scroll - 400);
            } else if (e.type == EV_DOWN) {
                page_set_scroll(&p, p.scroll + 400);
            } else if (e.type == EV_SELECT && tile == 0 && (p.focus == row_pad || p.focus == row_ufs)) {
                pick = p.focus;
            } else if (e.type == EV_TAP && tile == 0) {
                pick = page_hit(&p, e.x, e.y, 0);
            }
        }
        if (tile == 0 && pick >= 0 && pick == row_ufs) {
            if (!msc_busy())
                ufs_offer(!ufs_offered);
            next = 0;
        }
        if (tile == 0 && pick >= 0 && pick == row_pad) {
            ut_pad();
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            ui_header_draw(title, "Utility", "", 0);
            next = 0;
        }
        page_fling_step(&p);
    }
    page_free(&p);
}

static void open_tile(int i)
{
    if (i == T_LEAVE) {
        if (ui_dialog("Leave Utility?", "The phone restarts into the Starbite menu.",
                      ram_bytes ? "The RAM disk and its files are gone after the restart." : "",
                      "Restart", C_ORANGE))
            act(1);
        return;
    }
    if (i == T_FILES) {
        ut_files();
        return;
    }
    if (i == T_MEASURE) {
        ut_measure();
        return;
    }
    if (i == T_EVERYDAY) {
        ut_everyday();
        return;
    }
    if (i == T_REMOTE) {
        ut_remote();
        return;
    }
    if (i == T_SENSORS) {
        ut_sensors();
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
    pmsg_snapshot();                                  /* before this OS adds its own lines */
    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    ui_tick = ut_service;
    sb_log("Starbite Utility OS " UT_VERSION);
    wdt_feed();
    if (!sb_touch_start())
        sb_log("utility: no touch");
    find_serial();
    ramdisk_setup();
    wdt_feed();
    card_setup();
    if (ram_bytes)
        ramdisk_files();
    nlun_base = nlun;                                 /* RAM disk + card: what ufs_offer(0) returns to */
    msc_setup(lun, nlun);
    usb_routed = muic_route_usb();
    if (!usb_routed)
        sb_log("utility: MAX77705 did not answer (D+/D- route)");
    usb_start();
    ui_input_reset();
    ui_idle_restart();                                /* idle from here: the menu's time does not count */
    draw_home();
    ui_present();
    uint64_t next = sb_ms() + 1000;
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        int open = -1;
        if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = (focus + (e.type == EV_UP ? NTILE - 1 : 1)) % NTILE;
            draw_home();
        } else if (e.type == EV_SELECT) {
            open = focus;
        } else if (e.type == EV_TAP) {
            for (int i = 0; i < NTILE; ++i) {
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
            ut_status_bar();
            next = sb_ms() + 1000;
        }
        ui_present();
    }
}
