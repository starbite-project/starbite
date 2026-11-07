/* SPDX-License-Identifier: GPL-2.0-only
 * Wi-Fi test image (docs/WIFI-BCM4361.md, "The first phone test"): a bare-metal libsb image that
 * runs starbite/wifi's core on the phone: the BCM4361's power, PCIe channel 0's PHY and link, the
 * chip's ID and cores, its firmware and NVRAM from the phone's own VENDOR (read only), the msgbuf
 * rings and the first commands; with wifi_step=10 also "up" and a scan. Every line is on the screen
 * and in the pmsg ring (TWRP: /sys/fs/pstore/pmsg-ramoops-0).
 *
 * Boot arguments (the store entry's cmdline):
 *   wifi_step=N     the last step (1..10, default 9: rings and the first ioctls; bcmw.h BCMW_STEP_*)
 *   wifi_gen=1      train for Gen1 instead of Gen2 (the mainline port links at Gen1)
 *   wifi_auto=1     start without the held Volume Up (an unattended run the user agreed to)
 *   wifi_return=S   S seconds after the end, reboot to the boot menu (default: wait for Power)
 * Watchdog: fed (as the Utility OS feeds it) when the loader armed it (catalog wdt, wdt_feed "own"): a hang
 * resets into the loader's crash screen, and the radio's GPIOs fall with the reset.
 * Writes: PCIe channel 0's own registers, the BCM4361's (through its BARs), WLAN_REG_ON and PERST#
 * (gpg0-5, gpf2-2), CLKREQ#'s function (gpf2-0), the PMU's PCIE_PHY_CONTROL bit 0, CMU_FSYS1's
 * PCIe and data-path gates (the SysMMU's clock on, its registers untouched): bcmw_write_allowed()
 * refuses anything else. Nothing on storage. At the end (or Power) the radio is held in reset and
 * unpowered.
 */
#include "sb.h"
#include "sbui.h"
#include "catalog.h"
#include "store.h"
#include "../wifi/bcmw.h"
#include "../wifi/sb/bcmw_sb.h"

#define LINES 64U
#define COLS 200U
static char lines[LINES][COLS];
static unsigned nl;
static unsigned scan_aps, scan_done, events;
static uint64_t mem[(1U << 18) / 8];        /* struct bcmw (checked against bcmw_size()) */
static void wdt_feed(void);

static void draw(const char *title, const char *foot)
{
    int y = 140, px = ui_px(F_SMALL) + 6;
    unsigned first = nl > 52 ? nl - 52 : 0;

    ui_fill(0, 0, SB_FB_W, SB_FB_H, C_BG);
    ui_text(F_TITLE_S, 40, 100, title, C_L1);
    for (unsigned i = first; i < nl; i++) {
        const char *s = lines[i % LINES];
        while (*s && y < (int)SB_FB_H - 160) {
            unsigned n = ui_text_fit(F_SMALL, s, (int)SB_FB_W - 80);
            char part[COLS];
            if (n == 0)
                n = 1;
            for (unsigned k = 0; k < n && k + 1 < COLS; k++)
                part[k] = s[k];
            part[n < COLS ? n : COLS - 1] = 0;
            ui_text(F_SMALL, 40, y + px, part, sb_prefix(s, "fw: ") ? C_L2 : C_L1);
            y += px;
            s += n;
        }
    }
    if (foot)
        ui_text(F_ROW, 40, (int)SB_FB_H - 70, foot, C_STAR);
    ui_dirty_all();
    ui_present();
}

static void line(const char *s)
{
    wdt_feed();
    sb_strcpy(lines[nl % LINES], COLS, s);
    nl++;
    sb_log(s);
    draw("Wi-Fi test: BCM4361", 0);
}

static void os_log(void *ctx, const char *s)
{
    (void)ctx;
    line(s);
}

static void os_event(void *ctx, const struct bcmw_event *ev)
{
    (void)ctx;
    events++;
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        if (ev->status == 8 && ev->datalen >= 12)
            scan_aps += ev->data[10] | ev->data[11] << 8;   /* bss_count; names are not shown */
        else
            scan_done = 1;
    }
}

/* the cluster-1 watchdog, when the loader armed it: WTCNT = WTDAT, at most once a second (utility/main.c) */
static void wdt_feed(void)
{
    static uint64_t next;
    volatile uint32_t *wd = (volatile uint32_t *)0x10060000UL;
    uint64_t now = sb_ms();
    if (now < next)
        return;
    next = now + 1000;
    if (wd[0] & (1U << 5))
        wd[2] = wd[1];
}

/* a crash anywhere: the radio held in reset and unpowered, by the two GPIO data bits alone */
static void radio_off(void)
{
    volatile uint32_t *perst = (volatile uint32_t *)0x11430024UL, *reg_on = (volatile uint32_t *)0x10430084UL;
    *perst &= ~(1U << 2);
    *reg_on &= ~(1U << 5);
    __asm__ volatile("dsb sy" : : : "memory");
}

/* The settings: the launched catalog row's cmdline (the loader does not pass a bare entry's cmdline
 * on: it enters bare images with S-Boot's FDT unchanged; run 1 ran at the default step because of it),
 * then S-Boot's bootargs. key=value tokens, the first match wins. */
static const char *cat_line;

static int token(const char *s, const char *key, char *out, unsigned cap)
{
    unsigned kl = sb_strlen(key);
    while (s && *s) {
        const char *t;
        while (*s == ' ')
            s++;
        t = s;
        while (*s && *s != ' ')
            s++;
        if ((unsigned)(s - t) > kl && t[kl] == '=' && sb_prefix(t, key)) {
            unsigned o = 0;
            for (const char *v = t + kl + 1; v < s && o + 1 < cap; v++)
                out[o++] = *v;
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

static unsigned arg(const char *key, unsigned dflt)
{
    char v[16];
    unsigned n = 0;
    if (!token(cat_line, key, v, sizeof(v)) && !sb_bootarg(key, v, sizeof(v)))
        return dflt;
    for (unsigned i = 0; v[i] >= '0' && v[i] <= '9'; i++)
        n = n * 10 + (unsigned)(v[i] - '0');
    return n;
}

/* The key the loader launched: several hidden rows share this image (wifitest, wifitest-fw, wifitest-scan),
 * each with its own cmdline. 1. the loader's launch record, block 0 of the store's "crashlog" entry
 * (shim/starlte/sbfmt.h: "SBCRASH1", version 1, CRC32 of bytes 16..1023 at 12, launch_state 40 = 1 running,
 * method 44 = 5 bare, key at 48); 2. the last "STARBITE: launch <key> #" line of the pmsg ring, which the
 * loader writes right before it starts an OS; 3. "wifitest". */
static const char *launched_key(char key[24])
{
    struct store_ent e;
    static uint8_t blk[4096] __attribute__((aligned(64)));
    if (store_find("ufs:crashlog", &e) && e.size >= 4096 && !sb_blk_read(SB_MED_UFS, 0, e.lba, 1, blk)) {
        static const char magic[8] = { 'S', 'B', 'C', 'R', 'A', 'S', 'H', '1' };
        uint32_t crc = (uint32_t)blk[12] | (uint32_t)blk[13] << 8 | (uint32_t)blk[14] << 16 | (uint32_t)blk[15] << 24;
        if (!memcmp(blk, magic, 8) && blk[8] == 1 && sb_crc32(blk + 16, 1024 - 16) == crc && blk[40] == 1 &&
            blk[44] == 5 && blk[48]) {
            unsigned i = 0;
            for (; i < 23 && blk[48 + i]; i++)
                key[i] = (char)blk[48 + i];
            key[i] = 0;
            if (sb_prefix(key, "wifitest"))     /* a stale record of another OS is not ours */
                return "the loader's launch record";
        }
    }
    {
        /* the pmsg ring (log.c's layout): "DBGC", start, size, data from +12, 0x3F00 bytes */
        static const char pre[] = "STARBITE: launch ";
        volatile uint32_t *h = (volatile uint32_t *)SB_PMSG;
        volatile const char *d = (volatile const char *)(SB_PMSG + 12);
        uint32_t size = h[2] <= 0x3F00U ? h[2] : 0, start = h[1] % 0x3F00U, first = (start + 0x3F00U - size) % 0x3F00U;
        int found = 0;
        for (uint32_t k = 0; k + sizeof(pre) - 1 < size; k++) {
            unsigned m = 0;
            while (m < sizeof(pre) - 1 && d[(first + k + m) % 0x3F00U] == pre[m])
                m++;
            if (m == sizeof(pre) - 1) {
                unsigned i = 0;
                uint32_t at = k + m;
                while (i < 23 && at < size) {
                    char c = d[(first + at) % 0x3F00U];
                    if (c == ' ' || c == '\n' || c == 0)
                        break;
                    key[i++] = c;
                    at++;
                }
                key[i] = 0;
                found = i > 0 && sb_prefix(key, "wifitest");
            }
        }
        if (found)
            return "the last launch line in pmsg";
    }
    sb_strcpy(key, 24, "wifitest");
    return "neither the launch record nor pmsg said: the default";
}

static void settings(void)
{
    char key[24];
    const char *from = launched_key(key);
    int i = cat_load() > 0 ? cat_find(key) : -1;
    const struct cat_ent *e = i >= 0 ? cat_get((unsigned)i) : 0;
    struct ub u;
    cat_line = e ? e->cmdline : 0;
    ub_init(&u);
    ub_s(&u, "wifitest: settings: ");
    ub_s(&u, key);
    ub_s(&u, ": ");
    ub_s(&u, e ? (e->cmdline[0] ? e->cmdline : "(the catalog row has no cmdline)") : "(no catalog row with this key)");
    line(u.b);
    ub_init(&u);
    ub_s(&u, "wifitest: the key from ");
    ub_s(&u, from);
    line(u.b);
}

static int held_up(unsigned ms)
{
    uint64_t t0 = 0;
    for (;;) {
        unsigned k = sb_keys();
        if (k & SB_K_POWER)
            return 0;
        if (k & SB_K_UP) {
            if (!t0)
                t0 = sb_ms();
            if (sb_ms() - t0 >= ms)
                return 1;
        } else {
            t0 = 0;
        }
        wdt_feed();
        sb_mdelay(10);
    }
}

void sb_main(void)
{
    struct bcmw *w = (struct bcmw *)mem;
    struct bcmw_os os;
    struct bcmw_cfg cfg;
    unsigned step, autostart, ret, reached;
    int r;
    struct ub u;

    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    settings();
    step = arg("wifi_step", BCMW_STEP_RINGS);
    autostart = arg("wifi_auto", 0);
    ret = arg("wifi_return", autostart ? 30 : 0);       /* unattended: never wait forever */
    if (step < 1 || step > BCMW_STEP_ALL)
        step = BCMW_STEP_RINGS;
    ub_init(&u);
    ub_s(&u, "wifitest: up to step ");
    ub_dec(&u, step);
    ub_s(&u, step >= BCMW_STEP_FIRMWARE ? " (the firmware from VENDOR, read only)" : "");
    line(u.b);
    line("It powers the radio, trains PCIe channel 0 and talks to the chip. Nothing is written to storage.");
    line("At the end, or on Power, the radio is held in reset and unpowered.");
    if (!autostart) {
        draw("Wi-Fi test: BCM4361", "Hold Volume Up 2 s to start. Power: back.");
        if (!held_up(2000)) {
            line("wifitest: not started");
            sb_reboot(SB_INFORM_NORMAL);
        }
    }
    if (bcmw_size() > sizeof(mem)) {
        line("wifitest: struct bcmw does not fit");
        sb_reboot(SB_INFORM_NORMAL);
    }
    if (step >= BCMW_STEP_FIRMWARE && !bcmw_sb_vendor(line)) {
        line("wifitest: no files from VENDOR: only up to step 7 (the chip and its module)");
        step = BCMW_STEP_CHIP;
    }
    sb_crash_hook = radio_off;
    bcmw_sb_os(&os, os_log, os_event);
    memset(&cfg, 0, sizeof(cfg));
    cfg.stop_after = step;
    cfg.link_speed = arg("wifi_gen", 2);
    cfg.link_tries = 3;
    if (bcmw_init(w, &os, &cfg) != BCMW_OK) {
        line("wifitest: bcmw_init refused");
        sb_reboot(SB_INFORM_NORMAL);
    }
    r = bcmw_bringup(w, 0);
    if (r >= BCMW_STEP_RINGS) {
        uint64_t t0 = sb_ms();
        line("wifitest: polling for 10 s (events, the console)");
        while (sb_ms() - t0 < 10000 && !(sb_keys() & SB_K_POWER)) {
            bcmw_poll(w);
            wdt_feed();
            sb_mdelay(5);
        }
        ub_init(&u);
        ub_s(&u, "wifitest: ");
        ub_dec(&u, events);
        ub_s(&u, " events");
        if (step >= BCMW_STEP_UP) {
            ub_s(&u, "; the scan heard ");
            ub_dec(&u, scan_aps);
            ub_s(&u, scan_done ? " access points (complete)" : " access points (NOT complete)");
        }
        line(u.b);
    }
    reached = bcmw_step(w);
    bcmw_quiesce(w);
    sb_crash_hook = 0;
    ub_init(&u);
    ub_s(&u, "wifitest: END: reached step ");
    ub_dec(&u, reached);
    ub_s(&u, r < 0 ? " (stopped: the lines above say why)" : " (as asked)");
    line(u.b);
    draw("Wi-Fi test: BCM4361", ret ? "Rebooting to the menu soon. Power: now." : "Power: reboot to the boot menu.");
    {
        uint64_t t0 = sb_ms();
        sb_keys_release_wait();
        while (!(sb_keys() & SB_K_POWER) && !(ret && sb_ms() - t0 >= ret * 1000ULL)) {
            wdt_feed();
            sb_mdelay(20);
        }
    }
    sb_reboot(SB_INFORM_NORMAL);
}
