/* SPDX-License-Identifier: GPL-2.0-only
 * Modem probe image (docs/MODEM.md section 5, stage 2): a bare-metal libsb image that looks at the Shannon CP's
 * state; it changes nothing but step 4's two test words, which it puts back. Every line is on the screen and
 * in the pmsg ring (TWRP: /sys/fs/pstore/pmsg-ramoops-0).
 *
 *   1. the sbm core's self-test on the phone (in its own RAM: the SBD layout against this phone's logged
 *      offsets, a SIPC5 + IPC round trip, the request allow-list refusing every CALL / SMS command);
 *   2. step 1 (key "modemprobe"): PMU status registers and the CP mailbox (MCU_IPC) registers, read only;
 *   3. step 2 (key "modemprobe-shm"): also three small reads in the CP carve-out at 0xF0000000 (modem_if + 0,
 *      the memory map at + 0x2a0, the IPC region's first two words). TrustZone may refuse them (a synchronous
 *      abort or an SError: both are caught when they reach EL1; if EL3 takes the SError itself the phone may
 *      reset into the loader's crash screen). Step 2 runs after step 1's lines are in pmsg.
 *   4. step 3 (key "modemprobe-map", read only): the carve-out against the RADIO partition (GPT "RADIO", read
 *      only), range by range by CRC32: S-Boot's raw copy (TOC first) and cbd's places (MAIN + 0x10000, VSS +
 *      0x7900000).
 *   5. step 4 (key "modemprobe-acc", user present): CP_CTRL_S / CP_CTRL_NS through EL3's read call (SMC
 *      0x82000700, x1 = 3, as Samsung's PMUCAL), then a store test of two fixed carve-out words (the IPC region's
 *      unused command area, the CP memory's last word), each put back at once.
 *
 * Boot arguments (the catalog row's cmdline, then S-Boot's bootargs):
 *   modem_step=N    1..4 (default from the key: "modemprobe-shm" 2, "modemprobe-map" 3, "modemprobe-acc" 4)
 *   modem_auto=1    start without the held Volume Up (an unattended run the user agreed to)
 *   modem_return=S  S seconds after the end, reboot to the boot menu (default: 30 when auto, else wait for Power)
 * Watchdog: fed when the loader armed it (catalog wdt, wdt_feed "own"), as the Wi-Fi test image does.
 * Writes: no modem, PMU or partition register. Step 4 stores to two DRAM words in the carve-out and puts them
 * back (the only store path, refused for any other address). The watchdog feed (WTCNT, when the loader armed
 * it). Otherwise the screen, the log ring, RAM. No EL3 write call is ever made.
 * The CP is not booted, reset, powered or signalled. Nothing is dialled; no identifier is read.
 */
#include "sb.h"
#include "sbui.h"
#include "catalog.h"
#include "store.h"
#include "../sbm.h"
#include "../sb/sbm_sb.h"

#define LINES 64U
#define COLS 200U
static char lines[LINES][COLS];
static unsigned nl;
static int is_load;                     /* a modemload key: stage 3, not the read-only probe */
static void wdt_feed(void);

static void draw(const char *foot)
{
    int y = 140, px = ui_px(F_SMALL) + 6;
    unsigned first = nl > 52 ? nl - 52 : 0;

    ui_fill(0, 0, SB_FB_W, SB_FB_H, C_BG);
    ui_text(F_TITLE_S, 40, 100, is_load ? "Modem load: Shannon CP (stage 3 test)" : "Modem probe: Shannon CP (read only)",
            C_L1);
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
            ui_text(F_SMALL, 40, y + px, part, sb_prefix(s, "modemprobe: ") ? C_L1 : C_L2);
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
    draw(0);
}

static void linef(const char *fmt, unsigned a, unsigned b)
{
    char s[COLS];
    sbm_fmt(s, sizeof(s), fmt, a, b);
    sbm_redact_text(s, sizeof(s));
    line(s);
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

/* ---- settings: the launched catalog row's cmdline, then S-Boot's bootargs (as starbite/wifitest) -------- */

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

/* The key the loader launched: the loader's launch record (block 0 of the store's "crashlog": "SBCRASH1",
 * version 1, CRC32 of bytes 16..1023 at 12, launch_state 40 = 1 running, method 44 = 5 bare, key at 48), else the
 * last "STARBITE: launch <key> " line of the pmsg ring, else "modemprobe". */
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
            if (sb_prefix(key, "modemprobe") || sb_prefix(key, "modemload"))
                return "the loader's launch record";
        }
    }
    {
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
                found = i > 0 && (sb_prefix(key, "modemprobe") || sb_prefix(key, "modemload"));
            }
        }
        if (found)
            return "the last launch line in pmsg";
    }
    sb_strcpy(key, 24, "modemprobe");
    return "neither the launch record nor pmsg said: the default";
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

/* ---- 1. the core on this CPU, in ordinary RAM ------------------------------------------------------ */

static unsigned selftest(void)
{
    unsigned bad = 0;
    struct sbm_sbd s;
    uint8_t *ipc = sb_alloc(SBM_DT_IPC_SIZE);
    if (!ipc) {
        line("modemprobe: self-test: no RAM for the IPC model");
        return 1;
    }
    memset(ipc, 0, SBM_DT_IPC_SIZE);
    if (sbm_sbd_init(&s, ipc, SBM_DT_IPC_SIZE, sbm_starlte_chans, sbm_starlte_nchans) != 0)
        bad++;
    /* this phone's kernel log: RBP set at 3852, FMT1's DL vectors at 4692, RFS's DL buffers at 557056 */
    if (s.rbps_off != 3852 || s.r[2][1].addr_v_off != 4692 || s.r[3][1].buff_off != 557056 ||
        s.buff_end != 4276224)
        bad++;
    struct sbm_sbd cp;
    if (sbm_sbd_attach(&cp, ipc, SBM_DT_IPC_SIZE) != 0)
        bad++;
    uint8_t f[64];
    uint8_t rq[2] = { 0xff, SBM_DOMAIN_CS };
    int l = sbm_ipc_frame(f, sizeof(f), SBM_CH_FMT0, 0x42, SBM_NET_REGIST, SBM_IPC_GET, rq, 2);
    static const uint8_t want[] = { 0xf8, 0xeb, 0x0d, 0x00, 0x09, 0x00, 0x42, 0x00, 0x08, 0x05, 0x02, 0xff, 0x02 };
    if (l != 13 || memcmp(f, want, 13))
        bad++;
    if (l > 0 && sbm_sbd_put(&s, SBM_CH_FMT0, SBM_UL, f, (uint32_t)l, 0) != 0)
        bad++;
    const uint8_t *g;
    if (sbm_sbd_get(&cp, 1, SBM_UL, &g, 0) != 13 || memcmp(g, want, 13))
        bad++;
    unsigned refused = 0;
    for (unsigned i = 0; i < 256; i++)
        for (unsigned t = 0; t < 8; t++)
            refused += sbm_ipc_build(f, sizeof(f), 1, SBM_CMD(SBM_G_CALL, i), (uint8_t)t, 0, 0) == SBM_EPERM;
    if (refused != 256 * 8)
        bad++;
    {
        char s2[COLS];
        sbm_fmt(s2, sizeof(s2), "modemprobe: self-test %s (%u failed); %u CALL requests refused by the builder",
                bad ? "FAILED" : "ok", bad, refused);
        line(s2);
    }
    return bad;
}

static void plog(const char *s)
{
    line(s);
}

/* ---- stage 3 (docs/MODEM.md 6): keys modemload-dry / -sec / -run ------------------------------------ */

static struct sbm_plat park_plat;
static int park_armed;

/* sb_before_reset: every reboot path (the end, Power, the crash screen's reboot) parks a CP we touched */
static void park_hook(void)
{
    if (!park_armed)
        return;
    park_armed = 0;
    line("modemload: reset hook: parking the CP (Android's power-off) before the reboot");
    sbm_cp_park(&park_plat);
}

static void stage3(const char *key)
{
    static struct sbm_image im;
    struct sbm_boot b;
    struct sbm_probe pr;
    char s[COLS];
    enum sbm_boot_stop stop = sb_streq(key, "modemload-run") ? SBM_BOOT_RUN
                            : sb_streq(key, "modemload-sec") ? SBM_BOOT_SEC : SBM_BOOT_DRY;

    sb_storage_hold("modem test: no storage writes");
    sbm_fmt(s, sizeof(s), "modemload: %s; storage writes held; watchdog %u s (own: armed or fed here)",
            stop == SBM_BOOT_RUN ? "RUN: load, EL3 check, start the CP, first status, park"
            : stop == SBM_BOOT_SEC ? "SEC: load, EL3 check, park (the CP CPU is not started)" : "DRY: RAM only",
            sb_wdt_arm(30));
    line(s);
    line("modemload: no IPC is sent: no network search, no attach, no SIM use, no call of any kind");
    sbm_sb_plat(&park_plat, plog);
    sbm_probe_run(&park_plat, SBM_PROBE_REGS, &pr);
    sbm_probe_report(&park_plat, &pr);
    int r = sbm_sb_load_image(&im, line);
    if (r < 0) {
        sbm_fmt(s, sizeof(s), "modemload: loading RADIO / EFS failed: %s", sbm_strerror(r));
        line(s);
        return;
    }
    if (stop >= SBM_BOOT_SEC) {
        sbm_sb_plat_stage3(&park_plat);
        park_armed = 1;
        sb_before_reset = park_hook;
    }
    b.stop = stop;
    sbm_boot_run(&park_plat, &im, &b);
    park_armed = b.inited && !b.parked;  /* the core parked already; the hook only retries a failed park */
    sbm_boot_report(&park_plat, &b);
    line("modemload: after:");
    sbm_probe_run(&park_plat, SBM_PROBE_REGS, &pr);
    sbm_probe_report(&park_plat, &pr);
    if (stop >= SBM_BOOT_SEC) {
        uint32_t v, err;
        for (unsigned reg = 0; reg < 2; reg++)
            if (sbm_cp_ctrl_read(&park_plat, reg, &v, &err) == 0) {
                sbm_fmt(s, sizeof(s), "CTRL: %s = 0x%08x", reg ? "CP_CTRL_NS" : "CP_CTRL_S", v);
                line(s);
            }
    }
}

void sb_main(void)
{
    char key[24];
    struct sbm_plat p;
    struct sbm_probe pr;
    unsigned step, autostart, ret;

    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    {
        const char *from = launched_key(key);
        int i = cat_load() > 0 ? cat_find(key) : -1;
        const struct cat_ent *e = i >= 0 ? cat_get((unsigned)i) : 0;
        char s[COLS];
        cat_line = e ? e->cmdline : 0;
        sbm_fmt(s, sizeof(s), "modemprobe: key %s (from %s), cmdline: %s", key, from,
                e ? (e->cmdline[0] ? e->cmdline : "(none)") : "(no catalog row)");
        line(s);
    }
    step = arg("modem_step", sb_streq(key, "modemprobe-acc") ? 4 : sb_streq(key, "modemprobe-map") ? 3
                            : sb_streq(key, "modemprobe-shm") ? 2 : 1);
    if (step < 1 || step > 4)
        step = 1;
    autostart = arg("modem_auto", 0);
    ret = arg("modem_return", autostart ? 30 : 0);
    is_load = sb_prefix(key, "modemload");
    if (is_load) {
        line("modemload: stage 3 test (docs/MODEM.md 6). RADIO and EFS are only read; every partition write is held.");
        line("modemload: -sec / -run start the CP's power sequence through EL3 and always end with the CP parked.");
    } else {
        linef("modemprobe: step %u of %u (1 registers; 2 + carve-out reads; 3 + survey against RADIO; 4 + CP_CTRL"
              " via EL3 and a two-word store test)", step, 4);
        line("modemprobe: reads only. The CP is not booted, reset or signalled. No modem register or partition is written.");
    }
    if (!autostart) {
        draw("Hold Volume Up 2 s to start. Power: back.");
        if (!held_up(2000)) {
            line("modemprobe: not started");
            sb_reboot(SB_INFORM_NORMAL);
        }
    }
    selftest();
    if (is_load) {
        stage3(key);
        goto end;
    }

    sbm_sb_plat(&p, plog);
    {
        char s[COLS];
        sbm_fmt(s, sizeof(s), "modemprobe: faults so far: sync %u, SError %u", (unsigned)sb_fault.count,
                (unsigned)sb_fault.serror_count);
        line(s);
    }
    sbm_probe_run(&p, SBM_PROBE_REGS, &pr);
    sbm_probe_report(&p, &pr);
    if (step >= 2) {
        line("modemprobe: step 2: the CP carve-out (0xF0000000), stops at the first refused read");
        sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
        sbm_probe_report(&p, &pr);
    }
    if (step >= 3) {
        struct sbm_survey sv;
        line("modemprobe: step 3: the carve-out against the RADIO partition (read only)");
        sbm_probe_survey(&p, &sv);
        sbm_survey_report(&p, &sv);
    }
    if (step >= 4) {
        static struct sbm_wtest w[SBM_WTEST_N];
        uint32_t v, err;
        char s[COLS];
        line("modemprobe: step 4: CP_CTRL through EL3's read call, then the two-word store test");
        sbm_sb_plat_step4(&p);
        for (unsigned reg = 0; reg < 2; reg++) {
            sbm_fmt(s, sizeof(s), "probe: EL3 read call for %s", reg ? "CP_CTRL_NS" : "CP_CTRL_S");
            line(s);
            int r = sbm_cp_ctrl_read(&p, reg, &v, &err);
            sbm_fmt(s, sizeof(s), "CTRL: %s %s 0x%08x%s (EL3 error %u)", reg ? "CP_CTRL_NS" : "CP_CTRL_S",
                    r == 0 ? "=" : "read failed,", v,
                    r == 0 && reg == 0 ? ((v & 8) ? " (CP_START set)" : " (CP_START clear)")
                    : r == 0 ? ((v & 2) ? " (CP_PWRON set)" : " (CP_PWRON clear)") : "", err);
            line(s);
        }
        sbm_probe_write_test(&p, w);
        sbm_wtest_report(&p, w);
    }
end:
    {
        char s[COLS];
        if (sb_prefix(key, "modemload"))
            sbm_fmt(s, sizeof(s), "modemload: END; faults: sync %u, SError %u (stale before a read: %u)",
                    (unsigned)sb_fault.count, (unsigned)sb_fault.serror_count, sbm_sb_stale_serrors());
        else
            sbm_fmt(s, sizeof(s), "modemprobe: END: verdict %s; faults: sync %u, SError %u (stale before a read: %u)",
                    sbm_probe_verdict(&pr), (unsigned)sb_fault.count, (unsigned)sb_fault.serror_count,
                    sbm_sb_stale_serrors());
        line(s);
    }
    draw(ret ? "Rebooting to the menu soon. Power: now." : "Power: reboot to the boot menu.");
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
