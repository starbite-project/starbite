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
            if (sb_prefix(key, "modemprobe") || sb_prefix(key, "modemload") || sb_prefix(key, "modemlink") || sb_prefix(key, "modemsess") || sb_streq(key, "modemsim"))
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
                found = i > 0 && (sb_prefix(key, "modemprobe") || sb_prefix(key, "modemload") || sb_prefix(key, "modemlink") || sb_prefix(key, "modemsess") || sb_streq(key, "modemsim"));
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

/* modemsess*: the session API as Starbite OS uses it, from the small-footprint image (only nv_data in RAM; BOOT/MAIN/
 * VSS streamed from RADIO at each start): start, a 1 ms event loop for modem_hold seconds, the status GETs every
 * 10 s, a snapshot line in pmsg every 10 s and on the screen every second (masked: never the PLMN), then stop
 * (park). A stall or a crash is recovered as Starbite OS would (sbm_modem_recover). The guards park on a SIM or a
 * registration and stay parked.
 *   modemsess        one session, modem_hold s (120)
 *   modemsess-cycle  modem_cycles sessions (3) of modem_hold s (40) each, 6 s parked between: the same-boot restart
 *   modemsess-long   one session of modem_hold s (1500 = 25 min): continuous use */
static void snap_line(struct sbm_modem *mo, const char *tag, unsigned secs)
{
    struct sbm_modem_snap sn;
    char s[COLS];
    sbm_modem_snapshot(mo, &sn);
    sbm_fmt(s, sizeof(s), "%s +%u s: %s | state %u gen %u | q %u a %u open %u | rssi %d dBm | rx %u ms ago | starts %u"
            " restarts %u%s", tag, secs, sn.line, sn.state, sn.gen, sn.queries, sn.answers, sn.open, sn.rssi_dbm,
            sn.last_rx_ms == 0xFFFFFFFFu ? 0 : sn.last_rx_ms, sn.starts, sn.restarts, sn.emergency_only ?
            " | emergency calls only" : "");
    line(s);
}

static int session_run(struct sbm_modem *mo, unsigned secs, unsigned cycle)
{
    char s[COLS], st[COLS];
    uint64_t t0 = sb_ms(), last = t0, last_draw = 0, last_q = 0;
    while (sb_ms() - t0 < secs * 1000ULL) {
        uint64_t t = sb_ms();
        uint32_t el = (uint32_t)(t - last);
        last = t;
        uint64_t now = t - t0;
        if (mo->up) {
            int r = sbm_modem_poll(mo, el ? el : 1);
            if (r < 0) {
                sbm_fmt(s, sizeof(s), "modemsess: down: %s (parked)", sbm_strerror(r));
                line(s);
                snap_line(mo, "SESSION:", (unsigned)(now / 1000));
                if (mo->state == SBM_MS_GUARD)
                    return r;
            }
        } else {
            int r = sbm_modem_recover(mo, el);
            if (r == 0) {
                line("modemsess: recovered (restarted)");
                last = sb_ms();
            } else if (r < 0) {
                sbm_fmt(s, sizeof(s), "modemsess: no recovery: %s", sbm_strerror(r));
                line(s);
                return r;
            }
        }
        if (mo->up && now >= 3000 && now - last_q >= 10000) {
            last_q = now;
            unsigned q0 = mo->b.fmt_tx, a0 = mo->b.fmt_resp + mo->b.fmt_gen_res;
            int qr = sbm_modem_query(mo);
            sbm_fmt(s, sizeof(s), "SESSION: cycle %u +%u s: query round (q %u a %u before)%s", cycle,
                    (unsigned)(now / 1000), q0, a0, qr < 0 ? " | query error" : "");
            line(s);
            snap_line(mo, "SNAP:", (unsigned)(now / 1000));
        }
        if (now - last_draw >= 1000) {
            last_draw = now;
            sbm_modem_status(mo, st, sizeof(st));
            draw(st);
            sb_wdt_feed(0);
        }
        sb_udelay(1000);
    }
    return 0;
}

static void session(const char *key)
{
    static struct sbm_image im;
    static struct sbm_modem mo;
    char s[COLS];
    int cyc = sb_streq(key, "modemsess-cycle"), lng = sb_streq(key, "modemsess-long");
    unsigned secs = arg("modem_hold", cyc ? 40 : lng ? 1500 : 120), cycles = arg("modem_cycles", cyc ? 3 : 1);
    if (secs > 3600)
        secs = 3600;
    if (cycles < 1 || cycles > 5)
        cycles = 3;
    sb_storage_hold("modem session: no storage writes");
    sbm_fmt(s, sizeof(s), "modemsess: session API demo (%s), %u x %u s; watchdog %u s", key, cycles, secs,
            sb_wdt_arm(30));
    line(s);
    sbm_sb_plat(&park_plat, plog);
    uint64_t used0 = sb_arena_used();
    if (sbm_sb_load_image_stream(&im, line) < 0) {
        line("modemsess: loading RADIO / EFS failed");
        return;
    }
    sbm_fmt(s, sizeof(s), "modemsess: footprint: %u KiB of RAM for the modem (NV + its pristine copy + the stream chunk);"
            " the core's static buffers are in the image", (unsigned)((sb_arena_used() - used0) >> 10));
    line(s);
    sbm_sb_plat_stage4(&park_plat, im.mm.ipc_off);
    park_armed = 1;
    sb_before_reset = park_hook;
    for (unsigned c = 1; c <= cycles; ++c) {
        uint64_t t = sb_ms();
        int r = sbm_modem_start(&mo, &park_plat, &im);
        sbm_fmt(s, sizeof(s), "modemsess: cycle %u start: %s in %u ms (%s, streamed %u KiB; check %u ms, copy %u ms,"
                " link %u ms)", c, r == 0 ? "online" : sbm_strerror(r), (unsigned)(sb_ms() - t), mo.b.restart ?
                "restart: reset release" : "first start: cp_init", mo.b.streamed >> 10, mo.b.check_ms, mo.b.copy_ms,
                mo.b.link_ms);
        line(s);
        if (r != 0)
            break;
        r = session_run(&mo, secs, c);
        snap_line(&mo, "SESSION: end", secs);
        sbm_fmt(s, sizeof(s), "SESSION: end cycle %u: FMT %u RFS %u queries %u answers %u (unanswered %u stale %u"
                " unmatched %u by-command %u bad %u) stalls %u crashes %u", c, mo.b.fmt_rx, mo.b.rfs_rx, mo.b.fmt_tx,
                mo.b.fmt_resp + mo.b.fmt_gen_res, mo.b.fmt_tx - mo.b.fmt_resp - mo.b.fmt_gen_res, mo.b.fmt_stale,
                mo.b.fmt_unmatched, mo.b.fmt_seq_fixed, mo.b.bad_rx, mo.stalls, mo.crashes);
        line(s);
        sbm_modem_stop(&mo);
        line(mo.b.parked ? "modemsess: parked" : "modemsess: the park reported an error");
        if (r < 0 || !mo.b.parked)
            break;
        if (c < cycles)
            for (unsigned k = 0; k < 6; ++k) {      /* parked between the cycles, as a user's Stop / Start */
                sb_udelay(1000000);
                sb_wdt_feed(0);
            }
    }
    park_armed = 0;
}

/* modemsim: the ATTENDED real-SIM test (docs/MODEM.md 7.8). Only in modemsim.img, the one image built with
 * SBM_ATTENDED_TX; the session is marked attended, so the SIM and registration guards are off and the radio EXECs
 * are allowed. Nothing else is: no call, SMS, SS, SAT or identity request can be built in any image. The user is at
 * the phone with their SIM in. Steps: start (streamed), wait for the SIM, radio normal, wait for a registration,
 * hold sim_hold s with the signal, then low power mode (the CP detaches) and park. The log is masked (no PLMN, no
 * cell, no identity); the operator's name is drawn on the screen only. */
#ifdef SBM_ATTENDED_TX
static char sim_foot[COLS];
static struct sbm_net sim_net;
static struct sbm_modem *sim_mo;
static int sim_net_on;
static uint64_t sim_t0;

static int sim_ip_tx(void *ctx, const uint8_t *pkt, uint32_t len)
{
    (void)ctx;
    return sbm_modem_ip_tx(sim_mo, pkt, len);
}
static void sim_ip_rx(void *ctx, const uint8_t *pkt, uint32_t len)
{
    (void)ctx;
    if (sim_net_on)
        sbm_net_rx(&sim_net, pkt, len);
}

static int sim_registered(const struct sbm_modem_snap *sn)
{
    return sn->cs == SBM_REG_HOME || sn->cs == SBM_REG_ROAMING || sn->ps == SBM_REG_HOME || sn->ps == SBM_REG_ROAMING;
}

static int sim_ready(uint8_t s)
{
    return s == SBM_SIM_READY || s == SBM_SIM_INIT_COMPLETE || s == SBM_SIM_PB_INIT_COMPLETE;
}

/* the event loop for one phase: poll every 1 ms, the status GETs every q_ms, a masked SNAP line every 10 s, the
 * screen every second; returns 1 when done(sn) says so, 0 at max_ms, < 0 when the session went down */
static int sim_phase(struct sbm_modem *mo, const char *tag, uint32_t max_ms, uint32_t q_ms,
                     int (*done)(const struct sbm_modem *, const struct sbm_modem_snap *), uint32_t *took)
{
    uint64_t t0 = sb_ms(), last = t0, last_q = 0, last_snap = 0, last_draw = 0;
    struct sbm_modem_snap sn;
    char s[COLS];
    int first = 1;
    for (;;) {
        uint64_t t = sb_ms();
        uint32_t el = (uint32_t)(t - last), now = (uint32_t)(t - t0);
        last = t;
        int r = sbm_modem_poll(mo, el ? el : 1);
        if (sim_net_on)
            sbm_net_tick(&sim_net, (uint32_t)(sb_ms() - sim_t0));
        if (r < 0) {
            sbm_fmt(s, sizeof(s), "modemsim: %s: the session went down (%s); parked", tag, sbm_strerror(r));
            line(s);
            return r;
        }
        if (first || now - last_q >= q_ms) {
            first = 0;
            last_q = now;
            if (sbm_modem_query(mo) < 0) {
                sbm_fmt(s, sizeof(s), "modemsim: %s: a status query could not be sent", tag);
                line(s);
            }
        }
        sbm_modem_snapshot(mo, &sn);
        if (done && done(mo, &sn)) {
            *took = now;
            return 1;
        }
        if (now - last_snap >= 10000) {
            last_snap = now;
            sbm_fmt(s, sizeof(s), "SIM: %s +%u s: %s | bars %d | rssi %d dBm | q %u a %u", tag, now / 1000, sn.line,
                    sn.bars == 0xFF ? -1 : (int)sn.bars, sn.rssi_dbm, sn.queries, sn.answers);
            line(s);
        }
        if (now - last_draw >= 1000) {
            last_draw = now;
            const char *op = sn.have_plmn ? sbm_operator_name(sn.plmn) : 0;
            if (sn.have_plmn && !op)
                sbm_fmt(sim_foot, sizeof(sim_foot), "%s | network %s | %s | bars %d", tag, sn.plmn,
                        sbm_act_name(sn.rat), sn.bars == 0xFF ? -1 : (int)sn.bars);
            else
                sbm_fmt(sim_foot, sizeof(sim_foot), "%s | %s | %s | bars %d", tag, op ? op : "no network yet",
                        sbm_act_name(sn.rat), sn.bars == 0xFF ? -1 : (int)sn.bars);
            draw(sim_foot);                 /* the screen only: never logged */
            sb_wdt_feed(0);
        }
        if (now >= max_ms) {
            *took = now;
            return 0;
        }
        sb_udelay(1000);
    }
}

static int done_sim(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)mo;
    return sn->sim != 0xFF && (sim_ready(sn->sim) || sn->sim == SBM_SIM_LOCK_SC || sn->sim == SBM_SIM_LOCK_PN ||
                               sn->sim == SBM_SIM_ERROR);
}
static int done_radio(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)sn;
    return mo->b.radio_res_seen;
}
static int done_reg(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)mo;
    return sim_registered(sn);
}
static int done_ps(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)mo;
    return sn->ps == SBM_REG_HOME || sn->ps == SBM_REG_ROAMING;
}
static int done_define(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)sn;
    return mo->b.pdp.define_seen;
}
static int done_act(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)sn;
    return mo->b.pdp.act_seen;
}
static int done_ip(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)sn;
    return (mo->b.pdp.have_ip && mo->b.pdp.have_status && mo->b.pdp.status == 1) ||
           (mo->b.pdp.have_status && mo->b.pdp.status != 1 && mo->b.pdp.status_notis);
}
static int done_dns(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)mo;
    (void)sn;
    return sim_net.dns_state != SBM_NET_WAITING;
}
static int done_http(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)mo;
    (void)sn;
    return sim_net.http_state != SBM_NET_WAITING;
}
static int done_pdp_down(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    (void)sn;
    return mo->b.pdp.act_seen && mo->b.pdp.have_status && mo->b.pdp.status != 1;
}
static int res_ok(uint16_t code) { return (code & 0xFF00) == 0x8000; }

/* the data phase: PS attach if needed, define + activate cid 1 (the short form, the long one if refused), the IP
 * configuration, a DNS A query, an HTTP GET, deactivate. Returns 0 when all of it worked; *what says where not. */
static int sim_data(struct sbm_modem *mo, const char *apn, const char **what)
{
    char s[COLS];
    uint32_t took = 0;
    struct sbm_modem_snap sn;
    int r;
    sbm_modem_snapshot(mo, &sn);
    if (!(sn.ps == SBM_REG_HOME || sn.ps == SBM_REG_ROAMING)) {
        line("DATA: PS not registered: waiting 30 s for the CP's own attach");
        r = sim_phase(mo, "PS", 30000, 5000, done_ps, &took);
        if (r < 0) {
            *what = "PS (down)";
            return -1;
        }
        if (r == 0) {
            *what = "PS attach";
            return -1;
        }
    }
    sbm_fmt(s, sizeof(s), "DATA: APN \"%s\": define cid 1 (IPv4)", apn);
    line(s);
    if (sbm_modem_pdp_define(mo, apn) < 0 || (r = sim_phase(mo, "PDP", 10000, 10000, done_define, &took)) < 0) {
        *what = "define";
        return -1;
    }
    sbm_fmt(s, sizeof(s), "DATA: define answered 0x%04x after %u ms", mo->b.pdp.define_res, took);
    line(s);
    if (!mo->b.pdp.define_seen || !res_ok(mo->b.pdp.define_res)) {
        *what = "define (refused)";
        return -1;
    }
    for (int lf = 0; lf < 2; ++lf) {
        if (sbm_modem_pdp_activate(mo, 1, lf) < 0 || (r = sim_phase(mo, "PDP", 10000, 10000, done_act, &took)) < 0) {
            *what = "activate";
            return -1;
        }
        sbm_fmt(s, sizeof(s), "DATA: activate (%s form) answered 0x%04x after %u ms", lf ? "long" : "short",
                mo->b.pdp.act_res, took);
        line(s);
        if (mo->b.pdp.act_seen && res_ok(mo->b.pdp.act_res))
            break;
        if (lf) {
            *what = "activate (refused)";
            return -1;
        }
    }
    r = sim_phase(mo, "PDP", 30000, 10000, done_ip, &took);
    sbm_fmt(s, sizeof(s), "DATA: after %u ms: status %s %u (cause %u), IPv4 %s, DNS %s, MTU %u (addresses masked)",
            took, mo->b.pdp.have_status ? "" : "none", mo->b.pdp.status, mo->b.pdp.cause,
            mo->b.pdp.have_ip ? "assigned" : "none", mo->b.pdp.dns1[0] ? "given" : "none", mo->b.pdp.mtu);
    line(s);
    if (r < 0 || !mo->b.pdp.have_ip || mo->b.pdp.status != 1) {
        *what = r < 0 ? "PDP (down)" : "PDP up";
        return -1;
    }
    sim_mo = mo;
    sim_t0 = sb_ms();
    sbm_net_init(&sim_net, mo->b.pdp.ip, mo->b.pdp.dns1, mo->b.pdp.mtu, sim_ip_tx, 0, (uint32_t)sb_ms() * 2654435761u);
    sbm_modem_ip_rx_cb(sim_ip_rx, 0);
    sim_net_on = 1;
    static const char host[] = "connectivitycheck.gstatic.com";
    int ok = 1;
    if (sbm_net_dns(&sim_net, host, (uint32_t)(sb_ms() - sim_t0)) < 0)
        ok = 0;
    r = ok ? sim_phase(mo, "DNS", 12000, 10000, done_dns, &took) : -1;
    sbm_fmt(s, sizeof(s), "DATA: DNS %s for %s after %u ms (rcode %u, %u sent, rx %u, tx %u)",
            sim_net.dns_state == SBM_NET_DONE ? "answered" : "FAILED", host, took, sim_net.dns_rcode, sim_net.dns_sent,
            sim_net.rx_packets, sim_net.tx_packets);
    line(s);
    if (sim_net.dns_state == SBM_NET_DONE) {
        sbm_fmt(s, sizeof(s), "DATA: %s = %u.%u.%u.%u (a public address)", host, sim_net.dns_answer[0],
                sim_net.dns_answer[1], sim_net.dns_answer[2], sim_net.dns_answer[3]);
        line(s);
        if (sbm_net_http_get(&sim_net, sim_net.dns_answer, host, "/generate_204", (uint32_t)(sb_ms() - sim_t0)) == 0)
            r = sim_phase(mo, "HTTP", 20000, 10000, done_http, &took);
        sbm_fmt(s, sizeof(s), "DATA: HTTP GET /generate_204: %s, status %d, %u bytes, after %u ms (TCP %s, segments sent %u,"
                " out of order %u, bad csum %u)", sim_net.http_state == SBM_NET_DONE ? "done" : "FAILED",
                sim_net.http_code, sim_net.http_bytes, took, sim_net.tcp_connected ? "connected" : "no connection",
                sim_net.tcp_sent, sim_net.tcp_ooo, sim_net.rx_bad_csum);
        line(s);
    }
    sbm_fmt(s, sizeof(s), "DATA: rings: tx %u (errors %u), rx %u + zero-copy %u (bad %u)", mo->b.data_tx,
            mo->b.data_tx_err, mo->b.data_rx, mo->b.data_rx_zc, mo->b.data_rx_bad);
    line(s);
    sim_phase(mo, "HTTP", 1000, 10000, 0, &took);       /* the last ACKs */
    sim_net_on = 0;
    sbm_modem_ip_rx_cb(0, 0);
    if (sbm_modem_pdp_activate(mo, 0, 0) == 0)
        r = sim_phase(mo, "PDP", 10000, 10000, done_pdp_down, &took);
    sbm_fmt(s, sizeof(s), "DATA: deactivate answered 0x%04x; status %u after %u ms", mo->b.pdp.act_res,
            mo->b.pdp.status, took);
    line(s);
    if (sim_net.dns_state != SBM_NET_DONE) {
        *what = "DNS";
        return -1;
    }
    if (sim_net.http_state != SBM_NET_DONE || sim_net.http_code != 204) {
        *what = "HTTP";
        return -1;
    }
    return 0;
}

static int done_detached(const struct sbm_modem *mo, const struct sbm_modem_snap *sn)
{
    return mo->b.radio_res_seen && !sim_registered(sn);
}
#endif

static void simtest(const char *key)
{
#ifndef SBM_ATTENDED_TX
    (void)key;
    line("modemsim: this image has no attended transmit path: boot modemsim.img for the SIM test");
#else
    static struct sbm_image im;
    static struct sbm_modem mo;
    struct sbm_modem_snap sn;
    char s[COLS];
    uint32_t took = 0, reg_ms = 0, det_ms = 0;
    int registered = 0, data_done = 0;
    const char *data_fail = 0;
    uint8_t rat_reg = 0;
    unsigned reg_s = arg("sim_reg_s", 180), hold_s = arg("sim_hold", 60), bmin = 99, bmax = 0;
    const char *fail = 0;
    if (reg_s > 600)
        reg_s = 600;
    if (hold_s > 600)
        hold_s = 600;
    sb_storage_hold("modem SIM test: no storage writes");
    sbm_fmt(s, sizeof(s), "modemsim: ATTENDED SIM TEST (%s): SIM, radio on, registration, signal, then detach and park;"
            " watchdog %u s", key, sb_wdt_arm(30));
    line(s);
    line("modemsim: no call, no SMS, no data, no PIN entry. The log is masked: no PLMN, cell or identity.");
    sbm_sb_plat(&park_plat, plog);
    if (sbm_sb_load_image_stream(&im, line) < 0) {
        line("SIMTEST: FAIL at load: reading RADIO / EFS failed");
        return;
    }
    sbm_sb_plat_stage4(&park_plat, im.mm.ipc_off);
    park_armed = 1;
    sb_before_reset = park_hook;
    mo.attended = SBM_ATTENDED_MAGIC;
    uint64_t t = sb_ms();
    int r = sbm_modem_start(&mo, &park_plat, &im);
    sbm_fmt(s, sizeof(s), "modemsim: start: %s in %u ms (attended %s)", r == 0 ? "online" : sbm_strerror(r),
            (unsigned)(sb_ms() - t), mo.b.attended == SBM_ATTENDED_MAGIC ? "yes" : "NO");
    line(s);
    if (r != 0 || mo.b.attended != SBM_ATTENDED_MAGIC) {
        fail = r != 0 ? "start" : "attended mode";
        goto out;
    }

    /* 1. the SIM: ready, PIN, absent */
    r = sim_phase(&mo, "SIM", 60000, 5000, done_sim, &took);
    sbm_modem_snapshot(&mo, &sn);
    sbm_fmt(s, sizeof(s), "SIM: status 0x%02x (%s), key 0x%02x, after %u ms", sn.sim, sbm_sim_name(sn.sim),
            mo.b.cell.sim_key, took);
    line(s);
    if (r < 0) {
        fail = "SIM (down)";
        goto out;
    }
    if (sn.sim == SBM_SIM_ABSENT || sn.sim == 0xFF) {
        line("SIMTEST: FAIL at SIM: no SIM seen. Power off, insert the SIM, boot this test again.");
        fail = "";
        goto out;
    }
    if (sn.sim == SBM_SIM_LOCK_SC) {
        line("SIMTEST: STOP at SIM: the SIM wants its PIN (or PUK). This image enters no PIN: turn the PIN off in"
             " another phone or One UI, then run it again. Nothing was sent to the network.");
        fail = "";
        goto out;
    }
    if (!sim_ready(sn.sim)) {
        line("SIMTEST: STOP at SIM: the SIM is not ready (see the status above); nothing was sent to the network");
        fail = "";
        goto out;
    }
    line("SIM: ready");

    /* 2. radio normal (RIL's RadioPowerUpHandler) */
    if ((r = sbm_modem_radio(&mo, 1)) < 0) {
        sbm_fmt(s, sizeof(s), "modemsim: radio normal could not be sent (%s)", sbm_strerror(r));
        line(s);
        fail = "radio on";
        goto out;
    }
    r = sim_phase(&mo, "RADIO", 10000, 5000, done_radio, &took);
    sbm_fmt(s, sizeof(s), "RADIO: normal: %s 0x%04x after %u ms", mo.b.radio_res_seen ? "answered" : "no answer",
            mo.b.radio_res, took);
    line(s);
    if (r < 0) {
        fail = "radio on (down)";
        goto out;
    }

    /* 3. registration */
    r = sim_phase(&mo, "REG", reg_s * 1000u, 5000, done_reg, &reg_ms);
    sbm_modem_snapshot(&mo, &sn);
    if (r < 0) {
        fail = "registration (down)";
        goto out;
    }
    registered = r == 1;
    rat_reg = sn.rat;
    sbm_fmt(s, sizeof(s), "REG: %s after %u ms: CS %s, PS %s, %s (act 0x%02x); reject cause CS %u PS %u",
            r == 1 ? "registered" : "NOT registered", reg_ms, sn.cs == 0xFF ? "?" : sbm_reg_name(sn.cs),
            sn.ps == 0xFF ? "?" : sbm_reg_name(sn.ps), sbm_act_name(sn.rat), sn.rat, mo.b.cell.cs.reject,
            mo.b.cell.ps.reject);
    line(s);
    if (r == 1) {
        /* 4. hold: the signal while registered */
        uint64_t h0 = sb_ms();
        while (sb_ms() - h0 < hold_s * 1000ull) {
            r = sim_phase(&mo, "HOLD", 10000, 10000, 0, &took);
            if (r < 0)
                break;
            sbm_modem_snapshot(&mo, &sn);
            if (sn.bars != 0xFF) {
                bmin = sn.bars < bmin ? sn.bars : bmin;
                bmax = sn.bars > bmax ? sn.bars : bmax;
            }
        }
        if (r < 0) {
            fail = "hold (down)";
            goto out;
        }
        sbm_modem_snapshot(&mo, &sn);
        sbm_fmt(s, sizeof(s), "HOLD: %u s registered: CS %s, PS %s, %s, bars %u..%u, rssi %d dBm, still registered: %s",
                hold_s, sn.cs == 0xFF ? "?" : sbm_reg_name(sn.cs), sn.ps == 0xFF ? "?" : sbm_reg_name(sn.ps),
                sbm_act_name(sn.rat), bmin == 99 ? 0 : bmin, bmax, sn.rssi_dbm, sim_registered(&sn) ? "yes" : "no");
        line(s);
        line(sn.have_plmn ? (sbm_operator_name(sn.plmn) ? "HOLD: operator name known (on the screen only)"
                                                        : "HOLD: operator not in the table (its code is on the screen only)")
                          : "HOLD: no serving network reported");
    }

    /* 5. data (sim_data=1): PDP, DNS, HTTP, deactivate */
    if (registered && arg("sim_data", 1)) {
        char apn[SBM_APN_MAX + 1];
        if (!cat_line || !token(cat_line, "sim_apn", apn, sizeof(apn)) || !apn[0])
            sb_strcpy(apn, sizeof(apn), "internet");
        int dr = sim_data(&mo, apn, &data_fail);
        data_done = dr == 0;
        if (mo.up == 0) {
            fail = data_fail ? data_fail : "data (down)";
            goto out;
        }
    }

    /* 6. detach: low power mode, then park */
    if ((r = sbm_modem_radio(&mo, 0)) < 0) {
        sbm_fmt(s, sizeof(s), "modemsim: low power could not be sent (%s): parking", sbm_strerror(r));
        line(s);
        fail = "detach";
        goto out;
    }
    r = sim_phase(&mo, "DETACH", 20000, 3000, done_detached, &det_ms);
    sbm_modem_snapshot(&mo, &sn);
    sbm_fmt(s, sizeof(s), "DETACH: low power %s 0x%04x; %s after %u ms (CS %s, PS %s)",
            mo.b.radio_res_seen ? "answered" : "no answer", mo.b.radio_res,
            sim_registered(&sn) ? "STILL registered" : "not registered", det_ms, sn.cs == 0xFF ? "?" : sbm_reg_name(sn.cs),
            sn.ps == 0xFF ? "?" : sbm_reg_name(sn.ps));
    line(s);
    if (r != 1)
        fail = r < 0 ? "detach (down)" : "detach (timeout)";
    else if (!registered || bmax == 0)
        fail = registered ? "signal" : "registration";
    else if (arg("sim_data", 1) && !data_done)
        fail = data_fail ? data_fail : "data";
out:
    sbm_fmt(s, sizeof(s), "SIM: totals: FMT %u RFS %u queries %u answers %u (unanswered %u, by-command %u) stalls %u"
            " crashes %u", mo.b.fmt_rx, mo.b.rfs_rx, mo.b.fmt_tx, mo.b.fmt_resp + mo.b.fmt_gen_res,
            mo.b.fmt_tx - mo.b.fmt_resp - mo.b.fmt_gen_res, mo.b.fmt_seq_fixed, mo.stalls, mo.crashes);
    line(s);
    sbm_modem_stop(&mo);
    sbm_cp_park(&park_plat);                 /* safe again */
    line(mo.b.parked ? "modemsim: parked" : "modemsim: the park reported an error");
    if (!fail) {
        sbm_fmt(s, sizeof(s), "SIMTEST: PASS: SIM ready; registered in %u ms (%s); bars %u..%u; %s; detached in %u ms;"
                " parked", reg_ms, sbm_act_name(rat_reg), bmin, bmax, data_done ? "data: DNS + HTTP 204" : "no data test",
                det_ms);
        line(s);
    } else if (*fail) {
        sbm_fmt(s, sizeof(s), "SIMTEST: FAIL at %s; %s; parked", fail, registered ? "registration worked" : "not registered");
        line(s);
    }
    park_armed = 0;
#endif
}

static void stage3(const char *key)
{
    static struct sbm_image im;
    struct sbm_boot b;
    struct sbm_probe pr;
    char s[COLS];
    int soak = sb_streq(key, "modemlink-soak");
    int query = sb_streq(key, "modemlink-query") || soak;
    int serve = sb_streq(key, "modemlink-rfs") || query;
    enum sbm_boot_stop stop = sb_streq(key, "modemlink-online") || serve ? SBM_BOOT_ONLINE
                            : sb_prefix(key, "modemlink") ? SBM_BOOT_LINK
                            : sb_streq(key, "modemload-run") ? SBM_BOOT_RUN
                            : sb_streq(key, "modemload-sec") ? SBM_BOOT_SEC : SBM_BOOT_DRY;

    sbm_memset(&b, 0, sizeof(b));           /* 2ecf7cd left it unset: keep_running garbage, soak returned unparked */
    sb_storage_hold("modem test: no storage writes");
    sbm_fmt(s, sizeof(s), "modemload: %s; storage writes held; watchdog %u s (own: armed or fed here)",
            soak ? "SOAK: INIT_END, RFS from RAM, the 11 status GETs every 10 s, 10 min (no SIM: receive only), park"
            : query ? "ONLINE+RFS+QUERY: INIT_END, RFS from RAM, 11 status GETs (allow-listed, no SET), 60 s, park"
            : serve ? "ONLINE+RFS: stage 4, INIT_END, RFS answered from a RAM copy of nv_data.bin (FMT receive only), park"
            : stop == SBM_BOOT_ONLINE ? "ONLINE: stage 4, then INIT_END and 20 s receive only (no SIM allowed), park"
            : stop == SBM_BOOT_LINK ? "LINK: stage 3, then DL_START, UDL, INIT_START, PIF_INIT_DONE, PHONE_START, park"
            : stop == SBM_BOOT_RUN ? "RUN: load, EL3 check, start the CP, first status, park"
            : stop == SBM_BOOT_SEC ? "SEC: load, EL3 check, park (the CP CPU is not started)" : "DRY: RAM only",
            sb_wdt_arm(30));
    line(s);
    line(stop == SBM_BOOT_ONLINE ? "modemlink: INIT_END, then receive only: no request, no RFS answer; stops on a SIM, a registration or a crash"
         : stop == SBM_BOOT_LINK ? "modemlink: link commands only, never INIT_END: the CP never goes online (no RIL, network, SIM or call)"
                               : "modemload: no IPC is sent: no network search, no attach, no SIM use, no call of any kind");
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
        if (stop >= SBM_BOOT_LINK)
            sbm_sb_plat_stage4(&park_plat, im.mm.ipc_off);
        else
            sbm_sb_plat_stage3(&park_plat);
        park_armed = 1;
        sb_before_reset = park_hook;
    }
    b.stop = stop;
    b.hold_ms = sb_streq(key, "modemlink-hold") || stop == SBM_BOOT_ONLINE ? 1000u * arg("modem_hold", 20) : 0;
    b.rfs_serve = serve;
    b.fmt_query = query;
    if (query && !arg("modem_hold", 0))
        b.hold_ms = soak ? 600000 : 60000;
    b.query_period_ms = soak ? 10000 : 0;
    sbm_boot_run(&park_plat, &im, &b);
    if (b.inited && !b.parked) {         /* stage 3 never keeps the CP: park here, not only in the reset hook */
        line("modemload: the CP came back not parked: parking it now");
        b.parked = sbm_cp_park(&park_plat) == 0;
    }
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
    is_load = sb_prefix(key, "modemload") || sb_prefix(key, "modemlink") || sb_prefix(key, "modemsess") ||
              sb_streq(key, "modemsim");
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
    if (sb_prefix(key, "modemsess")) {
        session(key);
        goto end;
    }
    if (sb_streq(key, "modemsim")) {
        simtest(key);
        goto end;
    }
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
        if (is_load)
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
