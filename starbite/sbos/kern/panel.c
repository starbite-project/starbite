/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS panel power (docs/STARBITE-OS.md 3.3), v0.2. Both parts are off until Settings turns them on (a
 * phone test first; the settings last until a restart).
 *
 * 1. Refresh only on change. The loader leaves DECON_F's trigger free-running (HW_SW_TRIG_CONTROL 0x16030070 =
 *    0x1281: the panel is refreshed from DRAM at 60 Hz even when nothing changed; S-Boot itself leaves 0x3070,
 *    the trigger masked). The vendor driver runs this command-mode panel the same way: HW_TRIG_EN to send,
 *    HW_TRIG_MASK_DECON (bit 4) to stop (dpu_9810 decon_reg_set_trigger). Here: masked while the screen is
 *    still; every present unmasks it and a timer masks it again after 60 ms (at least two TE periods, one full
 *    transfer). The panel's own RAM keeps the picture.
 * 2. Panel off with the screen. The vendor's display-off / display-on sequences (s6e3ha8_star_a3_s0_panel.h:
 *    the level-1 key 9F A5 A5, DCS 0x28 / 0x29, the key closed with 9F 5A 5A), without sleep-in (0x10, whose
 *    way back needs the full panel init: build/em-research-brightness.md 3.4). DCS through DSIM 0x16080000 as
 *    Engineer OS's brightness page does it (payload FIFO, header, wait for the header FIFO to drain).
 * Never sent: sleep in, soft reset, POC / MTP / gamma / ELVSS / voltages. */
#include "sb.h"
#include "kern.h"

#define DECON_TRIG 0x16030070UL
#define TRIG_FREE 0x1281U
#define TRIG_MASK (1U << 4)
#define DSIM 0x16080000UL
#define DSIM_INTSRC (DSIM + 0x50)
#define DSIM_PKTHDR (DSIM + 0x58)
#define DSIM_PAYLOAD (DSIM + 0x5c)
#define DSIM_FIFOCTRL (DSIM + 0x68)
#define DSIM_PH_EMPTY (1U << 28)

int panel_save_on;                  /* settings: refresh only on change */
int panel_off_on;                   /* settings: the panel off with the screen */
static int masked, panel_dark;
static unsigned dcs_fail;
/* 0.6.1: Emergency Mode's power profile: refresh only on change and the panel off with the screen, whatever the
 * settings say (they stay as saved), and the brightness at most Mid (SOS and Extreme still boost to the top) */
static int em;
#define SAVE_ON (panel_save_on || em)
#define OFF_ON (panel_off_on || em)

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
}

static void trig(int mask)
{
    uint32_t v = rd(DECON_TRIG);
    wr(DECON_TRIG, mask ? (v | TRIG_MASK) : ((v & ~TRIG_MASK) | 1U));
    __asm__ volatile("dsb sy" ::: "memory");
    masked = mask;
}

/* a present happened: let the next TE carry it, mask again in 60 ms */
void panel_kick(void)
{
    if (!SAVE_ON || panel_dark)
        return;
    if (masked)
        trig(0);
    kt_set(KT_UI, 6, sb_ms(), 60, 0);
}

void panel_idle(void)
{
    if (SAVE_ON && !panel_dark && !masked)
        trig(1);
}

/* Settings turned the refresh saving off: free-running again (the loader's value) */
void panel_save_set(int on)
{
    panel_save_on = !!on;
    if (!on && !em) {
        wr(DECON_TRIG, TRIG_FREE);
        masked = 0;
    }
}

static int dsim_ready(void)
{
    return (rd(DSIM + 0x20) & (1U << 20)) && !(rd(DSIM + 0x4c) & (1U << 18));
}

static int dsim_wait(void)
{
    for (unsigned t = 0; t < 50000; t += 10) {
        uint32_t s = rd(DSIM_INTSRC), f = rd(DSIM_FIFOCTRL);
        if ((s & DSIM_PH_EMPTY) || ((f & (1U << 10)) && !((f >> 16) & 0x3fU))) {
            wr(DSIM_INTSRC, DSIM_PH_EMPTY);
            return 0;
        }
        sb_udelay(10);
    }
    return -1;
}

static void dsim_hdr(uint32_t id, uint32_t d0, uint32_t d1)
{
    uint32_t v = rd(DSIM_PKTHDR) & 0xfe000000U;
    wr(DSIM_PKTHDR, v | ((d1 & 0xffU) << 16) | ((d0 & 0xffU) << 8) | id);
}

static int dcs_long(const uint8_t *b, uint32_t n)
{
    if (((rd(DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_PH_EMPTY);
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < n; ++k)
            w |= (uint32_t)b[i + k] << (8 * k);
        wr(DSIM_PAYLOAD, w);
    }
    dsim_hdr(0x39, n & 0xffU, n >> 8);
    return dsim_wait();
}

static int dcs_short(uint8_t cmd)
{
    if (((rd(DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_PH_EMPTY);
    dsim_hdr(0x05, cmd, 0);
    return dsim_wait();
}

/* key, 0x28 or 0x29, key closed (always, even after a failure) */
static int display(int on)
{
    static const uint8_t key_on[3] = { 0x9f, 0xa5, 0xa5 }, key_off[3] = { 0x9f, 0x5a, 0x5a };
    if (!dsim_ready())
        return -1;
    int r = dcs_long(key_on, 3);
    if (!r)
        r = dcs_short(on ? 0x29 : 0x28);
    (void)dcs_long(key_off, 3);
    if (r) {
        ++dcs_fail;
        sb_log(on ? "panel: display on did not go out" : "panel: display off did not go out");
    }
    return r;
}

void panel_screen(int on)
{
    if (!on) {
        /* with the panel off: dark first, then no more transfers; without it the black frame the UI presents
         * next goes out through panel_kick like any other (the trigger is masked 60 ms later) */
        if (OFF_ON && !display(0)) {
            panel_dark = 1;
            if (SAVE_ON && !masked)
                trig(1);
        }
        return;
    }
    if (panel_dark) {
        trig(0);                                /* the current picture into the panel's RAM first */
        sb_mdelay(40);
        if (!display(1))
            panel_dark = 0;
        else {
            sb_mdelay(20);
            if (!display(1))                    /* once more */
                panel_dark = 0;
        }
        panel_dark = 0;                         /* never stay dark on purpose: the UI redraws anyway */
    }
    if (SAVE_ON)
        panel_kick();
    else if (masked)
        trig(0);
}

/* Track A: a call at the ear (kern_next.c's in-call proximity): the panel dark (display off) while something is near,
 * with the screen otherwise on; the frames keep going into the panel's RAM, so the display on shows the current one.
 * 1 done, 0 the command did not go out. */
int panel_blank(int on)
{
    if (display(on ? 0 : 1))
        return 0;
    if (!on && SAVE_ON)
        panel_kick();                           /* the next TE carries a fresh frame */
    return 1;
}

/* 3. Brightness (0.3): S-Boot's own AOR dimming, as Engineer OS's Tests > Brightness sends it
 * (build/em-research-brightness.md): F0 5A 5A, B1 hi lo, F7 03, F0 A5 A5, DCS long writes. Never gamma, ELVSS or
 * voltages. Level 0 sends nothing (the boot's own level stays); the panel keeps the value until the next OS
 * initialises it. A larger AOR value is darker. */
int panel_bright_level;                     /* the user's level (Settings, the control center; saved) */
static int applied = -1, boost;             /* the level on the panel; who asks for the top (1 Extreme, 2 SOS) */

static int send_level(int level);

/* what the panel shows for the user's level: the panel's top while boosted (5: AOR 00 0C, the vendor's UI maximum,
 * 400 nits, the shortest emission-off time Samsung's driver ever sends; not HBM); at most Mid (2) in Emergency Mode
 * ("Boot" is S-Boot's normal, brighter than Mid) */
static int effective(int level)
{
    if (boost)
        return 5;
    if (em && (level == 0 || level > 2))
        return 2;
    return level;
}

/* 0.6.5: a DCS read (the kernel's dsim_read flow, build/em-research-brightness.md 4): the return size, the read with
 * the bus turnaround, the answer from the RX FIFO. Frames are paused around it. Returns the bytes read or -1. */
static int dcs_read(uint8_t reg, uint8_t *buf, unsigned n)
{
    if (n > 64 || !dsim_ready())
        return -1;
    if (((rd(DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait())
        return -1;
    wr(DSIM_INTSRC, 1U << 18);                          /* RX_DATA_DONE */
    wr(DSIM_INTSRC, DSIM_PH_EMPTY);
    dsim_hdr(0x37, n, 0);                               /* set the maximum return size */
    if (dsim_wait())
        return -1;
    uint32_t v = rd(DSIM_PKTHDR) & 0xfe000000U;
    wr(DSIM_PKTHDR, v | (1U << 24) | ((uint32_t)reg << 8) | 0x06U);   /* DCS read, BTA */
    unsigned t = 0;
    for (; t < 100000 && !(rd(DSIM_INTSRC) & (1U << 18)); t += 10)
        sb_udelay(10);
    if (t >= 100000)
        return -1;
    wr(DSIM_INTSRC, 1U << 18);
    int got = 0;
    for (unsigned k = 0; k < 64 && !(rd(DSIM_FIFOCTRL) & (1U << 12)); ++k) {
        uint32_t w = rd(DSIM + 0x60);
        uint32_t type = w & 0xffU;
        if (type == 0x1c || type == 0x1a) {             /* a long answer: the word count, then the data */
            unsigned wc = (w >> 8) & 0xffffU;
            for (unsigned i = 0; i < wc; i += 4) {
                uint32_t d = rd(DSIM + 0x60);
                for (unsigned b = 0; b < 4 && i + b < wc; ++b)
                    if ((unsigned)got < n)
                        buf[got++] = (uint8_t)(d >> (8 * b));
            }
        } else if (type == 0x21 || type == 0x11) {
            buf[got++] = (uint8_t)(w >> 8);
        } else if (type == 0x22 || type == 0x12) {
            buf[got++] = (uint8_t)(w >> 8);
            if ((unsigned)got < n)
                buf[got++] = (uint8_t)(w >> 16);
        } else if (type == 0x02) {
            sb_logx("panel: read: an error report from the panel ", (w >> 8) & 0xffffU);
        }
    }
    return got;
}

static void log_hex(const char *what, const uint8_t *b, int n)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "panel: ");
    ub_s(&u, what);
    if (n < 0) {
        ub_s(&u, ": no answer");
    } else {
        ub_s(&u, ":");
        for (int i = 0; i < n; ++i) {
            ub_c(&u, ' ');
            ub_hex(&u, b[i], 2);
        }
    }
    sb_log(u.b);
}

/* 0.6.5: read only (Settings > About): the panel's ID and the registers HBM would need (gamma CA, the HBM gamma B3,
 * ELVSS B5, VINT F4, IRC B8 from offset 18); logged for the HBM decision. Writes only the level keys and B0 offsets. */
int panel_dump(void)
{
    static const uint8_t k1on[3] = { 0x9f, 0xa5, 0xa5 }, k1off[3] = { 0x9f, 0x5a, 0x5a };
    static const uint8_t k2on[3] = { 0xf0, 0x5a, 0x5a }, k2off[3] = { 0xf0, 0xa5, 0xa5 };
    static const uint8_t k3on[3] = { 0xfc, 0x5a, 0x5a }, k3off[3] = { 0xfc, 0xa5, 0xa5 };
    static const uint8_t off18[2] = { 0xb0, 18 };
    if (!dsim_ready()) {
        sb_log("panel: read: the link is not in command mode: nothing read");
        return 0;
    }
    int was_masked = masked;
    trig(1);                                            /* no frames during the reads */
    sb_mdelay(20);
    uint8_t b[64];
    int n = dcs_read(0x04, b, 3);
    log_hex("ID (04)", b, n);
    int ok = n == 3;
    if (!dcs_long(k1on, 3) && !dcs_long(k2on, 3) && !dcs_long(k3on, 3)) {
        n = dcs_read(0xca, b, 34);
        log_hex("gamma (CA)", b, n);
        n = dcs_read(0xb3, b, 34);
        log_hex("HBM gamma (B3)", b, n);
        n = dcs_read(0xb5, b, 23);
        log_hex("ELVSS (B5)", b, n);
        n = dcs_read(0xf4, b, 6);
        log_hex("VINT (F4)", b, n);
        n = dcs_read(0xb1, b, 2);
        log_hex("AOR (B1)", b, n);
        if (!dcs_long(off18, 2)) {
            n = dcs_read(0xb8, b, 17);
            log_hex("IRC (B8 from 18)", b, n);
        }
    } else {
        ok = 0;
    }
    (void)dcs_long(k3off, 3);
    (void)dcs_long(k2off, 3);
    (void)dcs_long(k1off, 3);
    if (!was_masked)
        trig(0);
    else
        panel_kick();
    sb_log(ok ? "panel: registers read (read only)" : "panel: the register read did not complete");
    return ok;
}

/* 1.2-alpha5 (probe H1): the ACL registers Samsung's brightness sequence also sends (55 on/off, B4 control 6 bytes;
 * the vendor's STAR_A3_S0_ACL_ONOFF / ACL_CONTROL), read only, for the HBM decision */
int panel_dump_acl(void)
{
    static const uint8_t k2on[3] = { 0xf0, 0x5a, 0x5a }, k2off[3] = { 0xf0, 0xa5, 0xa5 };
    if (!dsim_ready()) {
        sb_log("panel: read: the link is not in command mode: nothing read");
        return 0;
    }
    int was_masked = masked;
    trig(1);
    sb_mdelay(20);
    uint8_t b[16];
    int ok = 0;
    if (!dcs_long(k2on, 3)) {
        int n = dcs_read(0x55, b, 1);
        log_hex("ACL on/off (55)", b, n);
        ok = n == 1;
        n = dcs_read(0xb4, b, 6);
        log_hex("ACL control (B4)", b, n);
        ok &= n == 6;
    }
    (void)dcs_long(k2off, 3);
    if (!was_masked)
        trig(0);
    else
        panel_kick();
    return ok;
}

/* 0.6.5: the panel's own low-power mode (ALPM, 10 nits), Samsung's star_a3_s1 enter / exit tables exactly
 * (build/em-research-panel-hbm-lowpower.md 2.2): the picture in the panel's RAM stays; frames may still be sent.
 * Every enter is paired with an exit; the exit re-sends our AOR (the enter replaced it with 0B 74). */
int panel_in_lpm;

static int seq(const uint8_t *const *cmds, const uint8_t *lens, unsigned n)
{
    int r = 0;
    for (unsigned i = 0; i < n && !r; ++i)
        r = dcs_long(cmds[i], lens[i]);
    return r;
}

int panel_lpm(int on)
{
    static const uint8_t k2on[3] = { 0xf0, 0x5a, 0x5a }, k2off[3] = { 0xf0, 0xa5, 0xa5 };
    static const uint8_t k3on[3] = { 0xfc, 0x5a, 0x5a }, k3off[3] = { 0xfc, 0xa5, 0xa5 };
    static const uint8_t aor[3] = { 0xb1, 0x0b, 0x74 }, upd[2] = { 0xf7, 0x03 };
    static const uint8_t nit[7] = { 0xbb, 0x07, 0x0c, 0x9b, 0xee, 0x4f, 0x84 };     /* ALPM 10 nits */
    static const uint8_t avs_off[2] = { 0xb0, 0x2b }, avs[2] = { 0xfd, 0x03 };
    static const uint8_t mode[2] = { 0x53, 0x02 }, mode_off[2] = { 0x53, 0x00 };
    static const uint8_t avc2_off[6] = { 0xf4, 0xeb, 0x23, 0x0b, 0x17, 0x8a }, avc2_on[6] = { 0xf4, 0xeb, 0x23, 0x0b, 0x17, 0xca };
    static const uint8_t bb_exit[2] = { 0xbb, 0x03 };
    on = !!on;
    if (on == panel_in_lpm)
        return 1;
    if (!dsim_ready())
        return 0;
    int r;
    if (on) {
        const uint8_t *a[] = { k2on, aor, upd, k2off };
        const uint8_t al[] = { 3, 3, 2, 3 };
        r = seq(a, al, 4);
        if (!r) {
            sb_mdelay(17);                  /* one frame at 60 Hz */
            const uint8_t *b[] = { k2on, nit, k3on, avs_off, avs, k3off, mode, avc2_off, upd };
            const uint8_t bl[] = { 3, 7, 3, 2, 2, 3, 2, 6, 2 };
            r = seq(b, bl, 9);
        }
        (void)dcs_long(k3off, 3);
        (void)dcs_long(k2off, 3);           /* the keys closed, even after a failure */
        panel_in_lpm = 1;                   /* a partial enter is undone by the exit too */
    } else {
        const uint8_t *a[] = { k2on, bb_exit, avc2_on, mode_off };
        const uint8_t al[] = { 3, 2, 6, 2 };
        r = seq(a, al, 4);
        (void)dcs_long(k2off, 3);
        if (!r)
            panel_in_lpm = 0;
        applied = -2;                       /* our AOR again (the enter replaced it) */
        (void)send_level(effective(panel_bright_level) ? effective(panel_bright_level) : 0);
    }
    if (r)
        ++dcs_fail;
    sb_log(on ? (r ? "panel: the low-power mode's enter did NOT go out (the exit follows at the wake)" :
                     "panel: low-power mode (ALPM, 10 nits): the dim clock")
              : (r ? "panel: the low-power mode's exit did NOT go out" : "panel: low-power mode off, our brightness back"));
    if (on && !r && SAVE_ON)
        panel_kick();
    return !r;
}

void panel_emergency(int on)
{
    on = !!on;
    if (on == em)
        return;
    em = on;
    if (!em && !panel_save_on) {
        wr(DECON_TRIG, TRIG_FREE);          /* free-running again, as the settings say */
        masked = 0;
    }
    int eff = effective(panel_bright_level);
    if (eff != applied && !(eff == 0 && applied < 0))
        (void)send_level(eff);
    sb_log(on ? "panel: Emergency Mode's power profile: refresh only on change, off with the screen, at most Mid"
              : "panel: Emergency Mode's power profile off (the settings as saved)");
}

/* 0.6: the user's level; the panel shows it unless SOS or Extreme asks for the top level */
int panel_brightness(int level)
{
    if (level < 0 || level > 4)
        return 0;
    int was = panel_bright_level;
    panel_bright_level = level;
    int eff = effective(level);
    if (eff == applied || (eff == 0 && applied < 0))
        return 1;                           /* already there (or the boot's level, nothing sent yet) */
    if (!send_level(eff)) {
        panel_bright_level = was;
        return 0;
    }
    return 1;
}

/* SOS and Extreme: the panel's top level while either runs, the user's level after (never saved as the user's) */
void panel_boost(int who, int on)
{
    int b = on ? (boost | who) : (boost & ~who);
    if (b == boost)
        return;
    boost = b;
    int eff = effective(panel_bright_level);
    if (eff != applied && !(eff == 0 && applied < 0))
        (void)send_level(eff);
}

static int send_level(int level)
{
    static const uint16_t aor[6] = { 0x0440, 0x0b7c, 0x0800, 0x0440, 0x0200, 0x000c };   /* 5: the boost only */
    if (level < 0 || level > 5)
        return 0;
    static const uint8_t key_on[3] = { 0xf0, 0x5a, 0x5a }, commit[2] = { 0xf7, 0x03 }, key_off[3] = { 0xf0, 0xa5, 0xa5 };
    /* level 0 after a change is S-Boot's normal AOR (0x0440) */
    if (!dsim_ready())
        return 0;
    const uint8_t b1[3] = { 0xb1, (uint8_t)(aor[level] >> 8), (uint8_t)aor[level] };
    int r = -1;
    for (int attempt = 0; attempt < 2 && r; ++attempt) {
        r = dcs_long(key_on, 3);
        if (!r)
            r = dcs_long(b1, 3);
        if (!r)
            r = dcs_long(commit, 2);
        (void)dcs_long(key_off, 3);             /* always lock again */
        if (r)
            sb_mdelay(20);
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "settings: brightness ");
    ub_dec(&u, (uint64_t)level);
    ub_s(&u, r ? " not sent (DSI)" : " sent, AOR ");
    if (!r)
        ub_hex(&u, aor[level], 4);
    sb_log(u.b);
    if (r) {
        ++dcs_fail;
        return 0;
    }
    applied = level;
    if (SAVE_ON)
        panel_kick();                           /* the next refresh carries it */
    return 1;
}

unsigned panel_dcs_failures(void)
{
    return dcs_fail;
}
