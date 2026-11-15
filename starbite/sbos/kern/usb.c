/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: USB device mode (usb.h; docs/drivers/USB.md). The phone on a computer's cable shows up as one CDC-ACM
 * serial port, a read-only console: the kernel log streamed out, `help`, `ver`, `bat`, `log [N]`, `follow on|off`,
 * `usb`. Nothing writes storage or changes a setting; no drive, no partition, no EFS / PARAM is ever offered.
 *
 * What runs the hardware is the Utility OS's code, linked as it is (build.sh): starbite/utility/usb_dwc3.c (the
 * polling DWC3 device driver for the Exynos 9810 USB DRD at 0x10c00000, run as a USB 2.0 high-speed device: one event
 * buffer with its interrupt masked, one TRB per endpoint) and usb_phy.c (the three USB clock gates 0x1a242064,
 * 0x1100206c, 0x11002008, the PMU isolation 0x1406072c / 0x14060704 and the vendor CAL's USBCON 0x11100000 sequence).
 * Its DMA window is USB_DMA_BASE 0xa7000000 + 1 MiB, in libsb's non-cacheable map (mmu.c 0xa0000000..0xa8000000),
 * which nothing else in Starbite OS uses (the radio stops at 0xa6800000, bcmw_sb.c asserts it; the guest's bounce
 * buffers stop at 0xa6000000). The driver owns its first 16 KiB; this file's two buffers sit at +0x8000 / +0x9000.
 * usbacm.c gives that driver Starbite OS's descriptors (CDC-ACM only, 1209:0001 bcdDevice 3.00, serial "SBOS").
 *
 * This file is the glue:
 *  - the cable: the MAX77705 USB-C block (I2C 0x25 on libsb's HSI2C bus, sb_usbc_read) BC_STATUS 0x08: VBUSDET bit 7,
 *    ChgTyp bits 1:0 (Samsung's include/linux/muic/max77705-muic.h). A computer (SDP / CDP), or a cable whose type
 *    stays unknown for 2 s, starts the link; a wall charger (DCP) does not; Survival's USB power out (the phone as
 *    the source) is never taken for a computer. Read every 500 ms while a cable is in, 1 s otherwise; out after two
 *    low readings in a row (the PHY has VBUS forced valid, so only the MAX77705 sees an unplug).
 *  - the D+/D- switch: the MAX77705's MCU owns it and a vendor kernel's shutdown leaves it open, so the host sees
 *    nothing however well the DWC3 runs. As the loader and the Utility OS (max77705_muic.c) and Samsung's MUIC driver
 *    on every host attach: opcode CONTROL1_WRITE (0x06) with COM_USB (0x09) into AP_DATAOUT0/1 (0x21/0x22), the end
 *    marker AP_DATAOUT32 (0x41) = 0, through libsb's gate (sb_max_write, tier 1; only those three mailbox registers and
 *    the read-back's two: opcode 0x05 then AP_DATAIN0/1 0x51/0x52), again 1.5, 3 and 6 s after the cable came in while
 *    no host has configured the phone (the MCU's own detection may reopen it). The value is fixed here, never the
 *    user's, as vib.c's haptic bits.
 *  - the link: a snapshot of the PHY / PMU / clock-gate registers before the first start of the boot ("as the loader
 *    left them"), then usb_phy_start + usbd_start; the teardown is usbd_stop (the loader's own stop: soft disconnect,
 *    every transfer ended, events off, the event buffer address cleared, core soft reset) and every one of those
 *    registers that differs from the snapshot written back (USBCON first, the PMU, the gates last). A start that
 *    fails is torn down the same way and tried again after 5, 10, 20, 40, 60 s; a link no host configures in 10 s is
 *    restarted (then 20, 40, 60 s), as the loader's background fastboot does.
 *  - the console: bulk OUT 0x02 (512-byte transfers) into a line editor; bulk IN 0x82 from a 16 KiB ring, at most
 *    4 KiB a transfer and never a whole number of packets (no zero-length packet needed). A terminal opening the port
 *    (DTR) gets a banner and the log from then on; `log` replays the ring.
 *
 * Cost: usb_timer runs every 2 ms while a host enumerates or output is queued, 10 ms when idle and configured, every
 * 500 ms / 1 s for the cable alone. One call handles at most 16 controller events and 32 log lines and one IN
 * transfer: typically under 0.1 ms (register reads, a few DEPCMDs of a few us each); the bring-up call itself is
 * about 12 ms (the PHY sequence ~2 ms, the core soft reset + 10 ms settle) and at most ~0.32 s when the soft reset or
 * run/stop never completes (usb_dwc3.c's own 200 ms / 100 ms limits); the stop about 2 ms (at most ~0.2 s).
 *
 * Seen on a phone: the same DWC3 / PHY / MAX77705 code enumerated on the Mac from the loader (fastboot + ACM) and
 * from the Utility OS (2026-11-07, 1209:0001). This glue and these descriptors: never run on the phone yet (host test
 * kern/host/usb-test.sh only). Not built: CDC-ECM / NCM networking (docs/drivers/USB.md: the plan). */
#include "sb.h"
#include "kern.h"
#include "survival.h"
#include "usb.h"
#include "usbacm.h"
#include "usbctl.h"                        /* 9f: USB Control, the console's authenticated half */
#include "usb_dwc3.h"
#include "usb_phy.h"

/* ---- the MAX77705 USB-C block ------------------------------------------------------------------------------------ */
#define USBC 0x25U
#define R_USBC_STATUS1 0x06U
#define R_BC_STATUS 0x08U
#define R_CC_STATUS0 0x0aU
#define R_DATAOUT0 0x21U
#define R_DATAOUT1 0x22U
#define R_DATAOUT32 0x41U
#define R_DATAIN0 0x51U
#define BC_VBUSDET 0x80U
#define BC_CHGTYP 0x03U
#define OP_CTRL1_READ 0x05U
#define OP_CTRL1_WRITE 0x06U
#define COM_USB 0x09U

/* ---- the DWC3, status registers the glue reads (usb_dwc3.c owns the rest) ---------------------------------------- */
#define DWC 0x10c00000UL
#define GCTL 0xc110U
#define GSNPSID 0xc120U
#define GUSB2PHYCFG 0xc200U
#define GEVNTADRLO 0xc400U
#define GEVNTADRHI 0xc404U
#define GEVNTSIZ 0xc408U
#define GEVNTCOUNT 0xc40cU
#define DCFG 0xc700U
#define DCTL 0xc704U
#define DEVTEN 0xc708U
#define DSTS 0xc70cU
#define DCTL_RUN_STOP (1U << 31)
#define DCTL_CSFTRST (1U << 30)
#define DSTS_HALTED (1U << 22)

/* ---- the PHY, PMU and clock gates usb_phy_start writes, in the order they are put back ---------------------------- */
#define USBCON 0x11100000UL
#define GATE_TOP 0x1a242064UL
#define GATE_REF 0x1100206cUL
#define GATE_DPPHY 0x11002008UL
#define GATE_MANUAL (1U << 20)
#define GATE_CGVAL (1U << 21)
#define NPHY 18U
#define HSP_TEST_RO (3U << 20)      /* LINESTATE: read only, not part of "as found" */

static uint64_t phy_addr(unsigned i)
{
    switch (i) {
    case 0: return USBCON + 0x11cU;     /* HSREWA_INT1_MASK */
    case 1: return USBCON + 0x110U;     /* HSREWA_HSTK */
    case 2: return USBCON + 0x10cU;     /* HSREWA_REFTO */
    case 3: return USBCON + 0x108U;     /* HSREWA_CTRL */
    case 4: return USBCON + 0x100U;     /* REWA_ENABLE */
    case 5: return USBCON + 0x08U;      /* LINK_PORT */
    case 6: return USBCON + 0x58U;      /* HSP_TUNE */
    case 7: return USBCON + 0x48U;      /* COMBO_PMA */
    case 8: return USBCON + 0x04U;      /* LINK_CTRL */
    case 9: return USBCON + 0x50U;      /* UTMI: FORCE_SUSPEND / SLEEP back before the PHY powers down */
    case 10: return USBCON + 0x54U;     /* HSP: COMMONONN */
    case 11: return USBCON + 0x5cU;     /* HSP_TEST: SIDDQ (PHY power) */
    case 12: return USBCON + 0x20U;     /* CLKRST: the PHY / link resets */
    case 13: return 0x14060704UL;       /* PMU USB3 / DP PHY isolation */
    case 14: return 0x1406072cUL;       /* PMU USB2 PHY isolation */
    case 15: return GATE_DPPHY;
    case 16: return GATE_REF;
    default: return GATE_TOP;
    }
}

static const char *phy_name(unsigned i)
{
    switch (i) {
    case 0: return "hsrewa_int1";
    case 1: return "hsrewa_hstk";
    case 2: return "hsrewa_refto";
    case 3: return "hsrewa_ctrl";
    case 4: return "rewa_enable";
    case 5: return "link_port";
    case 6: return "hsp_tune";
    case 7: return "combo_pma";
    case 8: return "link_ctrl";
    case 9: return "utmi";
    case 10: return "hsp";
    case 11: return "hsp_test";
    case 12: return "clkrst";
    case 13: return "pmu_usb3dp";
    case 14: return "pmu_usb2";
    case 15: return "gate_dpphy";
    case 16: return "gate_ref";
    default: return "gate_top";
    }
}

static uint32_t phy_mask(unsigned i) { return i == 11 ? ~HSP_TEST_RO : 0xffffffffU; }

/* ---- memory and registers ------------------------------------------------------------------------------------------ */
#ifdef SB_HOST
extern uint8_t usb_host_dma[0x2000];        /* the test's stand-in for the DMA window */
#define DMA_RX ((uintptr_t)usb_host_dma)
#define DMA_TX ((uintptr_t)usb_host_dma + 0x1000U)
void usb_host_wr(uint64_t a, uint32_t v);
static void reg_wr(uint64_t a, uint32_t v) { usb_host_wr(a, v); }
#else
#define DMA_RX (USB_DMA_BASE + 0x8000UL)
#define DMA_TX (USB_DMA_BASE + 0x9000UL)
_Static_assert(USB_DMA_BASE + 0x8000UL >= USB_DMA_BASE + USB_DMA_DRIVER, "the console's buffers meet the driver's");
_Static_assert(USB_DMA_BASE + 0xa000UL <= USB_DMA_BASE + USB_DMA_SIZE, "the console's buffers leave the window");
static void reg_wr(uint64_t a, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
#define RX_LEN 512U
#define TX_CHUNK 4096U
#define PH_IN USB_PHYS(USBACM_EP_IN)
#define PH_OUT USB_PHYS(USBACM_EP_OUT)

static uint32_t reg_rd(uint64_t a, int *ok)
{
    uint32_t v = 0;
    if (!sb_probe32(a, &v))
        *ok = 0;
    return v;
}

/* ---- state ---------------------------------------------------------------------------------------------------------- */
static struct {
    int enabled, probe, st;
    int vbus, chgtyp, lows, rd_fails, otg;
    uint8_t bc;
    uint64_t t_in, t_rd, t_up, t_retry, t_cfg;
    unsigned retry_ms, relink_ms, reroute_i;
    int core, configured;
    uint32_t starts, relinks, fails, plugs, resets;
    const char *why;
} L;

static struct {
    int pending, asked;
    uint64_t t;
    unsigned tries;
    int ctrl1;
    uint32_t n, fails;
} R;

static struct {
    uint32_t snap[NPHY];
    int snapped, touched;
} Y;

#define TXCAP 16384U
static struct {
    char tx[TXCAP];
    unsigned th, tn;
    char line[USBCTL_LINE_MAX];              /* 9f: an AUTH line is ~290 characters */
    unsigned n;
    int cr, open, follow, catchup, prompt, mark_set;
    char mark[128];
    uint32_t txb, rxb, dropped, lost;
} C;

static void logu(struct ub *u) { sb_log(u->b); }

/* ---- pure parts ----------------------------------------------------------------------------------------------------- */
int usb_decide(int vbus, int chgtyp, uint64_t ms_since_vbus, int otg_out)
{
    if (otg_out || !vbus)
        return USB_OFF;                     /* Survival's power out: our own VBUS, nobody to talk to */
    if (chgtyp == 3)
        return USB_CHARGER;
    if (chgtyp == 1 || chgtyp == 2)
        return USB_UP;
    return ms_since_vbus >= 2000 ? USB_UP : USB_WAIT;   /* detection still running, or a host it could not type */
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* the word at *p (lower case, up to 15 chars) and the position after it and its spaces */
static const char *word(const char *p, char *w)
{
    unsigned n = 0;
    while (*p == ' ' || *p == '\t')
        ++p;
    while (*p && *p != ' ' && *p != '\t') {
        if (n < 15)
            w[n++] = lower(*p);
        ++p;
    }
    w[n] = 0;
    while (*p == ' ' || *p == '\t')
        ++p;
    return p;
}

int usb_cmd_parse(const char *line, unsigned *arg)
{
    char w[16], a[16], x[16];
    const char *p = word(line, w);
    p = word(p, a);
    (void)word(p, x);
    *arg = 0;
    if (!w[0])
        return USB_CMD_NONE;
    if (!sb_streq(w, "help") && !sb_streq(w, "?") && !sb_streq(w, "ver") && !sb_streq(w, "version") &&
        !sb_streq(w, "bat") && !sb_streq(w, "battery") && !sb_streq(w, "usb") && !sb_streq(w, "log") &&
        !sb_streq(w, "follow"))
        return USB_CMD_UNKNOWN;
    if (x[0])
        return USB_CMD_BAD;                 /* no command takes two arguments */
    if (sb_streq(w, "help") || sb_streq(w, "?"))
        return a[0] ? USB_CMD_BAD : USB_CMD_HELP;
    if (sb_streq(w, "ver") || sb_streq(w, "version"))
        return a[0] ? USB_CMD_BAD : USB_CMD_VER;
    if (sb_streq(w, "bat") || sb_streq(w, "battery"))
        return a[0] ? USB_CMD_BAD : USB_CMD_BAT;
    if (sb_streq(w, "usb"))
        return a[0] ? USB_CMD_BAD : USB_CMD_USB;
    if (sb_streq(w, "log")) {
        unsigned v = 0;
        if (!a[0])
            return USB_CMD_LOG;
        for (unsigned i = 0; a[i]; ++i) {
            if (a[i] < '0' || a[i] > '9' || i >= 6)
                return USB_CMD_BAD;
            v = v * 10U + (unsigned)(a[i] - '0');
        }
        if (!v)
            return USB_CMD_BAD;
        *arg = v;
        return USB_CMD_LOG;
    }
    if (sb_streq(a, "on"))                  /* follow */
        *arg = 1;
    else if (!sb_streq(a, "off"))
        return USB_CMD_BAD;
    return USB_CMD_FOLLOW;
}

/* ---- PHY snapshot and restore --------------------------------------------------------------------------------------- */
/* before a start, while nothing of ours is in them: the registers as the loader (or S-Boot) left them (after a
 * restore they read the same again) */
static int phy_snapshot(void)
{
    if (Y.touched)
        return 1;
    int ok = 1;
    for (unsigned i = 0; i < NPHY; ++i)
        Y.snap[i] = reg_rd(phy_addr(i), &ok);
    Y.snapped = ok;
    if (!ok)
        return 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "usb: as found: gates ");
    ub_hex(&u, Y.snap[17], 8);
    ub_c(&u, ' ');
    ub_hex(&u, Y.snap[16], 8);
    ub_c(&u, ' ');
    ub_hex(&u, Y.snap[15], 8);
    ub_s(&u, ", pmu ");
    ub_hex(&u, Y.snap[14], 8);
    ub_c(&u, ' ');
    ub_hex(&u, Y.snap[13], 8);
    logu(&u);
    ub_init(&u);
    ub_s(&u, "usb: as found: usbcon utmi ");
    ub_hex(&u, Y.snap[9], 8);
    ub_s(&u, " hsp_test ");
    ub_hex(&u, Y.snap[11], 8);
    ub_s(&u, " clkrst ");
    ub_hex(&u, Y.snap[12], 8);
    logu(&u);
    return 1;
}

/* registers that differ from the snapshot now (-1: one did not read) */
static int phy_diff(void)
{
    int ok = 1, n = 0;
    for (unsigned i = 0; i < NPHY; ++i) {
        uint32_t v = reg_rd(phy_addr(i), &ok);
        if ((v ^ Y.snap[i]) & phy_mask(i))
            ++n;
    }
    return ok ? n : -1;
}

static void phy_restore(void)
{
    if (!Y.touched || !Y.snapped)
        return;
    unsigned n = 0;
    for (unsigned i = 0; i < NPHY; ++i) {
        int ok = 1;
        uint32_t v = reg_rd(phy_addr(i), &ok);
        if (ok && ((v ^ Y.snap[i]) & phy_mask(i))) {
            reg_wr(phy_addr(i), Y.snap[i]);
            ++n;
        }
    }
    Y.touched = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "usb: PHY, PMU and clock gates back as found (");
    ub_dec(&u, n);
    ub_s(&u, " registers written back)");
    logu(&u);
}

/* usbd_start failed after it set the event buffer up but before it counts as started (stage -3: ep0 refused):
 * usbd_stop would do nothing, so the same tail by hand (events off, no event buffer, core soft reset) */
static void core_scrub(void)
{
    int ok = 1;
    reg_wr(DWC + DEVTEN, 0);
    reg_wr(DWC + GEVNTADRLO, 0);
    reg_wr(DWC + GEVNTADRHI, 0);
    reg_wr(DWC + GEVNTSIZ, 1U << 31);
    reg_wr(DWC + DCTL, DCTL_CSFTRST);
    for (unsigned i = 0; i < 1000; ++i) {
        if (!(reg_rd(DWC + DCTL, &ok) & DCTL_CSFTRST) || !ok)
            break;
        sb_udelay(100);
    }
    reg_wr(DWC + GEVNTCOUNT, 0);
}

/* ---- the D+/D- switch (MAX77705 CONTROL1) ----------------------------------------------------------------------- */
static int max_w(uint8_t reg, uint8_t v)
{
    struct sb_auth au;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)USBC << 8 | reg, 1, SB_TIER1, 0, "usb: D+/D- to the AP (MAX77705)");
    /* a fixed value (Samsung's own COM_USB on every host attach), only the USB-C block's mailbox: as vib.c, the two
     * steps the tier needs are this file's, not a person's */
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    if (!sb_gate_open(&au)) {
        sb_logx("usb: the gate refused the MAX77705 write, reg ", reg);
        return 0;
    }
    int ok = sb_max_write(USBC, reg, v, SB_TIER1);
    sb_gate_close();
    return ok;
}

static void route_write(uint64_t now)
{
    ++R.n;
    R.ctrl1 = -1;
    R.pending = 0;
    if (!(max_w(R_DATAOUT0, OP_CTRL1_WRITE) && max_w(R_DATAOUT1, COM_USB) && max_w(R_DATAOUT32, 0))) {
        ++R.fails;
        sb_log("usb: the D+/D- switch could not be set (MAX77705): the computer may not see the phone");
        return;
    }
    R.pending = 1;                          /* the read-back: asked after 50 ms */
    R.asked = 0;
    R.t = now + 50;
}

static void route_step(uint64_t now)
{
    if (!R.pending || now < R.t)
        return;
    if (!R.asked) {
        if (!(max_w(R_DATAOUT0, OP_CTRL1_READ) && max_w(R_DATAOUT32, 0))) {
            R.pending = 0;
            sb_log("usb: the D+/D- switch read-back could not be asked (the write may still have taken)");
            return;
        }
        R.asked = 1;
        R.tries = 0;
        R.t = now + 20;
        return;
    }
    uint8_t a0 = 0, a1 = 0;
    if (sb_usbc_read(R_DATAIN0, &a0) && a0 == OP_CTRL1_READ && sb_usbc_read(R_DATAIN0 + 1U, &a1)) {
        R.pending = 0;
        R.ctrl1 = a1;
        sb_logx(a1 == COM_USB ? "usb: D+/D- to the AP (MAX77705 CONTROL1 reads back) "
                              : "usb: the D+/D- switch is NOT to the AP: MAX77705 CONTROL1 reads ",
                a1);
        return;
    }
    if (++R.tries >= 10) {
        R.pending = 0;
        sb_log("usb: the MAX77705 did not answer the CONTROL1 read in 100 ms (the write may still have taken)");
        return;
    }
    R.t = now + 10;
}

static unsigned reroute_ms(unsigned i) { return i == 0 ? 1500U : i == 1 ? 3000U : 6000U; }

/* ---- the console ------------------------------------------------------------------------------------------------- */
static void con_put(const char *s)
{
    for (; *s; ++s) {
        if (C.tn >= TXCAP) {
            ++C.dropped;
            continue;
        }
        C.tx[(C.th + C.tn) % TXCAP] = *s;
        ++C.tn;
    }
}

static void con_dec(uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    con_put(u.b);
}

static void con_prompt(void)
{
    char l[sizeof(C.line)];
    unsigned i = 0;
    for (; i < C.n && i + 1 < sizeof(l); ++i)
        l[i] = C.line[i];
    l[i] = 0;
    con_put("> ");
    con_put(l);
    C.prompt = 1;
}

static void con_clear(void)
{
    C.th = C.tn = 0;
    C.n = 0;
    C.cr = C.catchup = C.prompt = 0;
}

static void mark_newest(void)
{
    unsigned cnt = sb_log_count();
    C.mark_set = cnt != 0;
    if (cnt)
        sb_strcpy(C.mark, sizeof(C.mark), sb_log_line(cnt - 1));
}

static void con_open(void)
{
    con_clear();
    C.open = 1;
    C.follow = 0;                           /* 9f: nothing of the log before a session (follow on in one) */
    mark_newest();
    con_put("\r\n");
    usbctl_open(con_put);                   /* the challenge: only the handshake is answered until a session */
    con_prompt();
}

static void con_close(void)
{
    usbctl_close("the port closed");
    if (usbd_busy(PH_IN))
        usbd_cancel(PH_IN);                 /* nobody reads it any more */
    con_clear();
    C.open = 0;
}

/* new log lines (follow), or the ring from a point (catch-up): at most 32 a call, and only while the TX ring has room
 * (the rest waits in the log ring; if that wraps first, a "lost" line says so) */
static void log_pull(void)
{
    if (!C.follow && !C.catchup)
        return;
    unsigned cnt = sb_log_count(), from = 0, k = 0, i;
    int wrote = 0;
    if (!cnt)
        return;
    if (C.mark_set) {
        if (sb_streq(sb_log_line(cnt - 1), C.mark)) {
            C.catchup = 0;
            return;
        }
        int found = 0;
        for (i = cnt - 1; i-- > 0;)
            if (sb_streq(sb_log_line(i), C.mark)) {
                from = i + 1;
                found = 1;
                break;
            }
        if (!found) {
            ++C.lost;
            if (C.prompt)
                con_put("\r\x1b[K");
            C.prompt = 0;
            con_put("[log lines were lost: the ring wrapped before they could go out]\r\n");
            wrote = 1;
        }
    }
    for (i = from; i < cnt && k < 32 && TXCAP - C.tn > 300U; ++i, ++k) {
        const char *l = sb_log_line(i);
        if (C.prompt) {
            con_put("\r\x1b[K");            /* the prompt off, the line, the prompt back below */
            C.prompt = 0;
        }
        con_put(l);
        con_put("\r\n");
        sb_strcpy(C.mark, sizeof(C.mark), l);
        C.mark_set = 1;
        wrote = 1;
    }
    if (i >= cnt)
        C.catchup = 0;
    if (wrote && C.open)
        con_prompt();
}

static const char *state_words(void);

static void cmd_ver(void)
{
    con_put("Starbite OS " SBOS_VERSION ", up ");
    con_dec(sb_ms() / 1000U);
    con_put(" s; USB console 1209:");
    struct ub u;
    ub_init(&u);
    ub_hexn(&u, USBACM_PID, 4);
    con_put(u.b);
    con_put(" (read only)\r\n");
}

static void cmd_bat(void)
{
    struct sbos_battery b;
    battery_get(&b);
    if (!b.ok) {
        con_put("battery: not read yet\r\n");
        return;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "battery ");
    ub_fix(&u, (uint64_t)(b.pct_x10 < 0 ? 0 : b.pct_x10) * 100U, 1);
    ub_s(&u, " %, ");
    ub_dec(&u, (uint64_t)(b.mv < 0 ? 0 : b.mv));
    ub_s(&u, " mV, ");
    ub_sdec(&u, b.ma);
    ub_s(&u, " mA (average ");
    ub_sdec(&u, b.avg_ma);
    ub_s(&u, " mA), ");
    if (b.temp_c10 > -1000) {
        if (b.temp_c10 < 0)
            ub_c(&u, '-');
        ub_fix(&u, (uint64_t)(b.temp_c10 < 0 ? -b.temp_c10 : b.temp_c10) * 100U, 1);
        ub_s(&u, " C, ");
    }
    ub_s(&u, b.charger == 1 ? "charger USB" : b.charger == 2 ? "charger wireless" : "no charger");
    con_put(u.b);
    con_put("\r\n");
}

static void cmd_usb(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "usb: ");
    ub_s(&u, state_words());
    ub_s(&u, usbd_full_speed() ? "; full speed" : "; high speed");
    ub_s(&u, "; address ");
    ub_dec(&u, usbacm_seen.address);
    ub_s(&u, "; bus resets ");
    ub_dec(&u, L.resets);
    ub_s(&u, ", setups ");
    ub_dec(&u, usbacm_seen.setups);
    ub_s(&u, ", stalls ");
    ub_dec(&u, usbacm_seen.stalls);
    ub_s(&u, "; starts ");
    ub_dec(&u, L.starts);
    ub_s(&u, ", restarts ");
    ub_dec(&u, L.relinks);
    ub_s(&u, ", failures ");
    ub_dec(&u, L.fails);
    con_put(u.b);
    ub_init(&u);
    ub_s(&u, "\r\nusb: D+/D- switch ");
    if (R.ctrl1 < 0)
        ub_s(&u, "not read back");
    else
        ub_hex(&u, (unsigned)R.ctrl1, 2);
    ub_s(&u, "; sent ");
    ub_dec(&u, C.txb);
    ub_s(&u, " bytes, received ");
    ub_dec(&u, C.rxb);
    ub_s(&u, ", dropped ");
    ub_dec(&u, C.dropped);
    ub_s(&u, ", log lines lost ");
    ub_dec(&u, C.lost);
    con_put(u.b);
    con_put("\r\n");
}

static void con_exec(void)
{
    unsigned arg = 0;
    char l[sizeof(C.line)];
    unsigned i = 0;
    for (; i < C.n && i + 1 < sizeof(l); ++i)
        l[i] = C.line[i];
    l[i] = 0;
    if (!usbctl_authed()) {                 /* 9f: before a session, the handshake only */
        usbctl_line(l, con_put);
        return;
    }
    if (usbctl_exec(l, con_put))
        return;
    switch (usb_cmd_parse(l, &arg)) {
    case USB_CMD_NONE:
        break;
    case USB_CMD_HELP:
        con_put("commands (this console only reads: it changes nothing on the phone):\r\n"
                "  help           this list\r\n"
                "  ver            the version and the time since the start\r\n"
                "  bat            the battery\r\n"
                "  log [N]        the kernel log so far (or its last N lines)\r\n"
                "  follow on|off  new log lines as they are written (on when the port opens)\r\n"
                "  usb            this link\r\n");
        break;
    case USB_CMD_VER:
        cmd_ver();
        break;
    case USB_CMD_BAT:
        cmd_bat();
        break;
    case USB_CMD_LOG: {
        unsigned cnt = sb_log_count();
        if (!arg || arg >= cnt) {
            C.mark_set = 0;                 /* from the oldest line */
        } else {
            sb_strcpy(C.mark, sizeof(C.mark), sb_log_line(cnt - arg - 1));
            C.mark_set = 1;
        }
        C.catchup = 1;
        break;
    }
    case USB_CMD_FOLLOW:
        C.follow = (int)arg;
        if (arg)
            mark_newest();
        con_put(arg ? "follow on\r\n" : "follow off\r\n");
        break;
    case USB_CMD_USB:
        cmd_usb();
        break;
    case USB_CMD_BAD:
        con_put("bad arguments (help lists the commands)\r\n");
        break;
    default:
        con_put("unknown command (help lists them; this console only reads)\r\n");
        break;
    }
}

static void con_input(const volatile uint8_t *p, unsigned n)
{
    if (!C.open)
        con_open();                         /* a terminal that types without raising DTR */
    for (unsigned i = 0; i < n; ++i) {
        char c = (char)p[i];
        ++C.rxb;
        if (c == '\r' || c == '\n') {
            if (c == '\n' && C.cr) {
                C.cr = 0;
                continue;
            }
            C.cr = c == '\r';
            con_put("\r\n");
            C.prompt = 0;
            con_exec();
            C.n = 0;
            con_prompt();
            continue;
        }
        C.cr = 0;
        if (c == 0x7f || c == 0x08) {
            if (C.n) {
                --C.n;
                con_put("\b \b");
            }
        } else if (c == 0x03) {             /* ^C: the line and a running replay dropped */
            con_put("^C\r\n");
            C.n = 0;
            C.catchup = 0;
            con_prompt();
        } else if (c >= 0x20 && c < 0x7f && C.n + 1 < sizeof(C.line)) {
            char e[2] = { c, 0 };
            C.line[C.n++] = c;
            con_put(e);
        }
    }
}

static void tx_pump(void)
{
    if (!C.tn || usbd_busy(PH_IN))
        return;
    unsigned n = C.tn < TX_CHUNK ? C.tn : TX_CHUNK;
    unsigned mps = usbd_full_speed() ? 64U : 512U;
    if (!(n % mps))
        --n;                                /* never whole packets only: no zero-length packet to send */
    volatile uint8_t *d = (volatile uint8_t *)DMA_TX;
    for (unsigned i = 0; i < n; ++i)
        d[i] = (uint8_t)C.tx[(C.th + i) % TXCAP];
    if (usbd_queue(PH_IN, DMA_TX, n))
        return;
    C.th = (C.th + n) % TXCAP;
    C.tn -= n;
    C.txb += n;
}

/* ---- the link ---------------------------------------------------------------------------------------------------- */
static void acm_reset(void)
{
    usbctl_close("the USB link went down");
    L.configured = 0;
    C.open = 0;
    con_clear();
}

/* the controller stopped (as the loader leaves it), the PHY as found */
static void link_down(const char *why)
{
    if (L.core || Y.touched) {
        usbd_stop();
        L.core = 0;
        phy_restore();
        if (why && why[0])
            sb_logf("usb: stopped: ", why);
    }
    acm_reset();
    R.pending = 0;
    L.why = why ? why : "";
}

static void link_fail(uint64_t now, int stage, const char *why)
{
    ++L.fails;
    usbd_stop();
    if (stage == -3)
        core_scrub();
    L.core = 0;
    phy_restore();
    acm_reset();
    L.why = why ? why : "the USB start failed";
    L.st = USB_RETRY;
    L.t_retry = now + L.retry_ms;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "usb: did not start: ");
    ub_s(&u, L.why);
    ub_s(&u, " (stage ");
    ub_sdec(&u, stage);
    ub_s(&u, "); again in ");
    ub_dec(&u, L.retry_ms / 1000U);
    ub_s(&u, " s");
    logu(&u);
    L.retry_ms = L.retry_ms >= 30000U ? 60000U : L.retry_ms * 2U;
}

static void link_up(uint64_t now)
{
    const char *why = 0;
    ++L.starts;
    if (!phy_snapshot()) {
        ++L.fails;
        L.why = "the USB PHY registers did not read";
        L.st = USB_RETRY;
        L.t_retry = now + 60000U;
        sb_log("usb: the USB PHY / PMU / clock-gate registers did not read: USB stays off");
        return;
    }
    route_write(now);                       /* D+/D- to the AP first, as the loader */
    acm_reset();
    usbacm_seen_reset();
    Y.touched = 1;
    if (usb_phy_start(&why)) {
        link_fail(now, -10, why);
        return;
    }
    L.core = 1;
    int r = usbd_start(USBACM_SERIAL, &why);
    if (r) {
        link_fail(now, r, why);
        return;
    }
    L.st = USB_UP;
    L.t_up = now;
    L.why = "";
    sb_log("usb: up: the CDC-ACM console waits for the computer");
}

static void relink(uint64_t now)
{
    unsigned next = L.relink_ms >= 30000U ? 60000U : L.relink_ms * 2U;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "usb: no host configured the phone in ");
    ub_dec(&u, L.relink_ms / 1000U);
    ub_s(&u, " s (a Mac waiting for \"Allow accessory\"?): the link again");
    logu(&u);
    ++L.relinks;
    usbd_stop();
    L.core = 0;
    acm_reset();
    L.reroute_i = 3;                        /* the restart writes the switch itself */
    link_up(now);
    L.relink_ms = next;
}

static void on_event(const struct usbd_ev *e, uint64_t now)
{
    switch (e->type) {
    case USBD_EV_RESET:
        ++L.resets;
        L.configured = 0;
        if (C.open)
            con_close();
        break;
    case USBD_EV_DISCONNECT:
        L.configured = 0;
        if (C.open)
            con_close();
        break;
    case USBD_EV_CONFIGURED:
        L.configured = e->value == 1;
        if (L.configured) {
            L.t_cfg = now;
            L.relink_ms = 10000U;
            struct ub u;
            ub_init(&u);
            ub_s(&u, "usb: a computer configured the phone: address ");
            ub_dec(&u, usbacm_seen.address);
            ub_s(&u, usbd_full_speed() ? ", full speed" : ", high speed");
            logu(&u);
        } else if (C.open) {
            con_close();
        }
        break;
    case USBD_EV_LINE:
        if ((e->value & 1U) && !C.open)
            con_open();
        else if (!(e->value & 1U) && C.open)
            con_close();
        break;
    case USBD_EV_DONE:
        if (e->ep == PH_OUT)
            con_input((const volatile uint8_t *)DMA_RX, e->value < RX_LEN ? e->value : RX_LEN);
        break;
    default:
        break;
    }
}

/* the controller's events, the console's input armed, its output out: bounded (16 events, 32 log lines, 1 IN) */
static void service(uint64_t now)
{
    struct usbd_ev e;
    for (unsigned n = 0; n < 16 && usbd_poll(&e); ++n)
        on_event(&e, now);
    if (!L.configured)
        return;
    if (!usbd_busy(PH_OUT))
        (void)usbd_queue(PH_OUT, DMA_RX, RX_LEN);
    if (C.open) {
        log_pull();
        tx_pump();
    }
}

static void apply(int want, uint64_t now)
{
    switch (want) {
    case USB_OFF:
        if (L.st != USB_OFF)
            link_down(L.otg ? "Survival's USB power out is on" : "the cable is out");
        L.st = USB_OFF;
        L.retry_ms = 5000U;
        L.relink_ms = 10000U;
        break;
    case USB_CHARGER:
        if (L.st != USB_CHARGER) {
            link_down("a wall charger, not a computer");
            sb_log("usb: a wall charger (BC1.2 DCP): no data link");
        }
        L.st = USB_CHARGER;
        break;
    case USB_WAIT:
        if (L.st == USB_OFF || L.st == USB_CHARGER)
            L.st = USB_WAIT;
        break;
    default:
        if (L.st == USB_OFF || L.st == USB_CHARGER || L.st == USB_WAIT)
            link_up(now);
        break;
    }
}

static void vbus_poll(uint64_t now)
{
    uint8_t bc = 0;
    struct surv_usb sv;
    surv_usb_status(&sv);
    L.otg = sv.state == SURV_USB_ON;
    if (!sb_usbc_read(R_BC_STATUS, &bc)) {
        if (++L.rd_fails == 4 && L.st != USB_OFF) {
            link_down("the MAX77705 stopped answering (the cable cannot be watched)");
            L.st = USB_OFF;
            L.vbus = 0;
        }
        return;
    }
    L.rd_fails = 0;
    L.bc = bc;
    L.chgtyp = (int)(bc & BC_CHGTYP);
    if ((bc & BC_VBUSDET) && !L.otg) {
        L.lows = 0;
        if (!L.vbus) {
            L.vbus = 1;
            L.t_in = now;
            L.reroute_i = 0;
            ++L.plugs;
            sb_logx("usb: a cable is in (MAX77705 BC_STATUS) ", bc);
        }
    } else if (L.vbus && (L.otg || ++L.lows >= 2)) {
        L.vbus = 0;
        sb_log(L.otg ? "usb: VBUS is Survival's own power out" : "usb: the cable is out");
    }
    apply(usb_decide(L.vbus, L.chgtyp, now - L.t_in, L.otg), now);
}

/* one turn: the cable, the switch, the controller; the ms until the next turn */
static unsigned link_step(uint64_t now)
{
    if (now >= L.t_rd) {
        vbus_poll(now);
        L.t_rd = now + (L.vbus || L.st != USB_OFF ? 500U : 1000U);
    }
    if (L.st == USB_UP && L.vbus && !L.configured && L.reroute_i < 3 && now - L.t_in >= reroute_ms(L.reroute_i)) {
        ++L.reroute_i;
        if (!R.pending)
            route_write(now);
    }
    route_step(now);
    if (L.st == USB_RETRY && now >= L.t_retry)
        link_up(now);
    if (L.st == USB_UP) {
        service(now);
        if (!L.configured && now - L.t_up >= L.relink_ms)
            relink(now);
    }
    uint64_t w = L.t_rd > now ? L.t_rd - now : 1;
    if (L.st == USB_UP) {
        uint64_t s = !L.configured || C.tn || C.catchup ? 2U : 10U;
        if (s < w)
            w = s;
    }
    if (R.pending && R.t > now && R.t - now < w)
        w = R.t - now;
    if (L.st == USB_RETRY && L.t_retry > now && L.t_retry - now < w)
        w = L.t_retry - now;
    return w ? (unsigned)w : 1U;
}

/* ---- the kernel's calls -------------------------------------------------------------------------------------------- */
void usb_timer(unsigned id)
{
    (void)id;
    if (!L.enabled || L.probe)
        return;
    uint64_t now = sb_ms();
    unsigned w = link_step(now);
    usbctl_tick(now, C.open ? con_put : 0);   /* 9f: the pairing's answer, idle, a deferred restart */
    if (usbctl_authed() || C.open)
        w = w > 100U ? 100U : w;             /* (a pairing answer or a restart waits at most 0.1 s) */
    kt_set(KT_USB, 0, sb_ms(), w, 0);
}

void usb_enable(int on)
{
    if (on && !L.enabled && !L.probe) {
        L.enabled = 1;
        L.st = USB_OFF;
        L.t_rd = 0;
        L.retry_ms = 5000U;
        L.relink_ms = 10000U;
        sb_log("usb: watching the cable (USB device mode: the USB Control console)");
        kt_set(KT_USB, 0, sb_ms(), 1, 0);
    } else if (!on && L.enabled) {
        link_down("USB switched off");
        L.enabled = 0;
        L.st = USB_OFF;
        L.vbus = 0;
        kt_cancel(KT_USB, 0);
    }
}

void usb_init(void)
{
    R.ctrl1 = -1;
    if (sbos_selftest_enabled) {
        sb_log("usb: off in the self-test images (the drvprobe drives it there)");
        return;
    }
    if (!SBOS_USB_AUTO) {
        sb_log("usb: off (SBOS_USB_AUTO 0 until the drvprobe's level 1 has passed on the phone)");
        return;
    }
    usb_enable(1);
}

static const char *state_words(void)
{
    if (!L.enabled && !L.probe)
        return "USB off";
    switch (L.st) {
    case USB_CHARGER:
        return "Charger (no data)";
    case USB_WAIT:
        return "Cable in: checking";
    case USB_RETRY:
        return "USB did not start (trying again)";
    case USB_UP:
        return !L.configured ? "Waiting for the computer" : C.open ? "Computer: console open" : "Connected to a computer";
    default:
        return "Not connected";
    }
}

const char *usb_why(void) { return state_words(); }

void usb_status(struct usb_status *s)
{
    memset(s, 0, sizeof(*s));
    s->state = L.st;
    s->enabled = L.enabled;
    s->vbus = L.rd_fails ? -1 : L.vbus;
    s->chgtyp = L.chgtyp;
    s->configured = L.configured;
    s->terminal = C.open;
    s->full_speed = L.st == USB_UP ? usbd_full_speed() : 0;
    s->address = (int32_t)usbacm_seen.address;
    s->ctrl1 = R.ctrl1;
    s->starts = L.starts;
    s->relinks = L.relinks;
    s->fails = L.fails;
    s->plugs = L.plugs;
    s->tx_bytes = C.txb;
    s->rx_bytes = C.rxb;
    s->tx_dropped = C.dropped;
    s->why = L.why ? L.why : "";
}

/* ---- the drivers track probe (drvprobe.h) -------------------------------------------------------------------------- */
static struct {
    int level, phase, owned;
    uint64_t t0, t_rd;
} P;

static void plog(const char *s) { sb_logf("drvprobe: usb: ", s); }

static const char *chgtyp_words(unsigned t)
{
    return t == 1 ? "a computer (SDP)" : t == 2 ? "a charging port (CDP)" : t == 3 ? "a wall charger (DCP)" : "not typed";
}

static int gate_forced_off(uint32_t g) { return (g & GATE_MANUAL) && !(g & GATE_CGVAL); }

/* usb_dwc3.c's and usb_phy.c's own dumps, one log line each */
static char diag_buf[3072];
static void log_diag(void)
{
    struct tb t;
    tb_init(&t, diag_buf, sizeof(diag_buf));
    usbd_diag(&t);
    usb_phy_diag(&t);
    char *s = diag_buf;
    for (char *p = diag_buf;; ++p) {
        if (*p == '\n' || !*p) {
            int end = !*p;
            *p = 0;
            if (*s)
                plog(s);
            if (end)
                break;
            s = p + 1;
        }
    }
}

static int probe_level0(void)
{
    struct ub u;
    uint8_t bc = 0, s1 = 0, cc0 = 0;
    int mok = sb_usbc_read(R_BC_STATUS, &bc);
    if (mok) {
        (void)sb_usbc_read(R_USBC_STATUS1, &s1);
        (void)sb_usbc_read(R_CC_STATUS0, &cc0);
    }
    ub_init(&u);
    if (mok) {
        ub_s(&u, "MAX77705 USB-C: BC_STATUS ");
        ub_hex(&u, bc, 2);
        ub_s(&u, (bc & BC_VBUSDET) ? " (VBUS in, " : " (no VBUS, ");
        ub_s(&u, chgtyp_words(bc & BC_CHGTYP));
        ub_s(&u, "), USBC_STATUS1 ");
        ub_hex(&u, s1, 2);
        ub_s(&u, ", CC_STATUS0 ");
        ub_hex(&u, cc0, 2);
    } else {
        ub_s(&u, "MAX77705 USB-C block (I2C 0x25) did not answer");
    }
    plog(u.b);

    uint32_t v[NPHY];
    int pok = 1;
    for (unsigned i = 0; i < NPHY; ++i)
        v[i] = reg_rd(phy_addr(i), &pok);
    for (unsigned g = 0; g < NPHY / 3U; ++g) {
        ub_init(&u);
        for (unsigned i = g * 3U; i < g * 3U + 3U; ++i) {
            ub_s(&u, phy_name(i));
            ub_c(&u, ' ');
            ub_hex(&u, v[i], 8);
            ub_s(&u, i == g * 3U + 2U ? "" : ", ");
        }
        plog(u.b);
    }
    int gated = gate_forced_off(v[17]) || gate_forced_off(v[16]);
    int dok = 1, dread = 0;
    uint32_t id = 0, gctl = 0, dctl = 0, dsts = 0, dcfg = 0, eva = 0, evs = 0, u2 = 0;
    if (pok && !gated) {
        dread = 1;
        id = reg_rd(DWC + GSNPSID, &dok);
        gctl = reg_rd(DWC + GCTL, &dok);
        dctl = reg_rd(DWC + DCTL, &dok);
        dsts = reg_rd(DWC + DSTS, &dok);
        dcfg = reg_rd(DWC + DCFG, &dok);
        eva = reg_rd(DWC + GEVNTADRLO, &dok);
        evs = reg_rd(DWC + GEVNTSIZ, &dok);
        u2 = reg_rd(DWC + GUSB2PHYCFG, &dok);
        ub_init(&u);
        ub_s(&u, "DWC3: GSNPSID ");
        ub_hex(&u, id, 8);
        ub_s(&u, " GCTL ");
        ub_hex(&u, gctl, 8);
        ub_s(&u, " DCTL ");
        ub_hex(&u, dctl, 8);
        ub_s(&u, " DSTS ");
        ub_hex(&u, dsts, 8);
        plog(u.b);
        ub_init(&u);
        ub_s(&u, "DWC3: DCFG ");
        ub_hex(&u, dcfg, 8);
        ub_s(&u, " GEVNTADRLO ");
        ub_hex(&u, eva, 8);
        ub_s(&u, " GEVNTSIZ ");
        ub_hex(&u, evs, 8);
        ub_s(&u, " GUSB2PHYCFG ");
        ub_hex(&u, u2, 8);
        plog(u.b);
        if (dok) {
            unsigned cap = (gctl >> 12) & 3U;
            ub_init(&u);
            ub_s(&u, "DWC3 as found: ");
            ub_s(&u, cap == 1 ? "host" : cap == 2 ? "device" : cap == 3 ? "OTG" : "unset");
            ub_s(&u, (dctl & DCTL_RUN_STOP) ? ", RUNNING (run/stop on)" : ", run/stop off");
            ub_s(&u, (dsts & DSTS_HALTED) ? ", halted" : ", not halted");
            ub_s(&u, eva ? ", an event buffer set" : ", no event buffer (the loader's usbd_stop state)");
            plog(u.b);
        }
    } else if (pok) {
        plog("DWC3 not read: its clock gate is forced off (a read could hang the bus)");
    }
    if (L.enabled && L.st != USB_OFF)
        plog("note: the USB service runs: the registers above are its, not the loader's");

    ub_init(&u);
    if (!mok) {
        ub_s(&u, "FAIL the MAX77705 USB-C block did not answer on I2C (the cable cannot be watched)");
    } else if (!pok) {
        ub_s(&u, "FAIL the USB PHY / PMU / clock-gate registers did not read");
    } else if (dread && !dok) {
        ub_s(&u, "FAIL a DWC3 register read faulted");
    } else if (dread && (id >> 16) != 0x5533U && (id >> 16) != 0x3331U) {
        ub_s(&u, "FAIL no DWC3 at 0x10c00000 (GSNPSID ");
        ub_hex(&u, id, 8);
        ub_c(&u, ')');
    } else {
        ub_s(&u, "PASS ");
        if (dread) {
            ub_s(&u, "DWC3 id ");
            ub_hex(&u, id, 8);
            ub_s(&u, (dctl & DCTL_RUN_STOP) ? " running" : " stopped");
        } else {
            ub_s(&u, "DWC3 clock gated (not read)");
        }
        ub_s(&u, (v[11] & (1U << 24)) ? ", PHY powered down" : ", PHY powered");
        ub_s(&u, (bc & BC_VBUSDET) ? ", VBUS in from " : ", no VBUS");
        if (bc & BC_VBUSDET)
            ub_s(&u, chgtyp_words(bc & BC_CHGTYP));
    }
    plog(u.b);
    return 1;
}

static void probe_reset(void)
{
    P.level = 0;
    P.phase = 0;
}

/* the probe gave the link back: the service (if switched on) watches the cable again */
static void probe_release(void)
{
    acm_reset();
    R.pending = 0;
    L.probe = 0;
    L.st = USB_OFF;
    L.vbus = 0;
    P.owned = 0;
    probe_reset();
    if (L.enabled) {
        L.t_rd = 0;
        kt_set(KT_USB, 0, sb_ms(), 1, 0);
    }
}

/* level 1's end: the controller dumped, stopped and checked while its clock still runs (only if it ran: a failed start
 * was already torn down by link_fail, with the gates maybe forced off again), then the PHY back as found */
static int probe_finish(const char *why_end)
{
    struct usbacm_seen seen = usbacm_seen;
    int cfg = L.configured || seen.config == 1, ran = L.core;
    int dok = 1, core_ok = 1;
    uint32_t dctl = 0, dsts = 0, eva = 0;
    if (ran) {
        log_diag();
        usbd_stop();
        L.core = 0;
        dctl = reg_rd(DWC + DCTL, &dok);
        dsts = reg_rd(DWC + DSTS, &dok);
        eva = reg_rd(DWC + GEVNTADRLO, &dok);
        core_ok = dok && !(dctl & DCTL_RUN_STOP) && !eva;
    }
    phy_restore();
    int diff = phy_diff();
    int full = usbd_full_speed();
    probe_release();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "after the stop: DCTL ");
    ub_hex(&u, dctl, 8);
    ub_s(&u, " DSTS ");
    ub_hex(&u, dsts, 8);
    ub_s(&u, " GEVNTADRLO ");
    ub_hex(&u, eva, 8);
    ub_s(&u, "; registers not as found ");
    ub_sdec(&u, diff);
    plog(u.b);
    ub_init(&u);
    ub_s(&u, "host: resets ");
    ub_dec(&u, L.resets);
    ub_s(&u, ", setups ");
    ub_dec(&u, seen.setups);
    ub_s(&u, ", descriptors ");
    ub_dec(&u, seen.get_desc);
    ub_s(&u, ", SET_ADDRESS ");
    ub_dec(&u, seen.set_address);
    ub_s(&u, " (");
    ub_dec(&u, seen.address);
    ub_s(&u, "), SET_CONFIG ");
    ub_dec(&u, seen.set_config);
    ub_s(&u, ", D+/D- ");
    if (R.ctrl1 < 0)
        ub_s(&u, "not read back");
    else
        ub_hex(&u, (unsigned)R.ctrl1, 2);
    plog(u.b);
    ub_init(&u);
    if (why_end) {
        ub_s(&u, "FAIL ");
        ub_s(&u, why_end);
    } else if (!core_ok) {
        ub_s(&u, "FAIL the DWC3 did not stop cleanly (run/stop or the event buffer left set)");
    } else if (diff != 0) {
        ub_s(&u, "FAIL the PHY / PMU / clock-gate registers are not back as found");
    } else if (!seen.setups) {
        ub_s(&u, "FAIL up for 10 s but the host sent nothing (no SETUP: D+/D- not routed, or no pull-up seen)");
    } else if (!cfg) {
        ub_s(&u, "FAIL the host read ");
        ub_dec(&u, seen.get_desc);
        ub_s(&u, seen.set_address ? " descriptors, set an address, never configured it (new accessory?)"
                                  : " descriptors but never set an address");
    } else {
        ub_s(&u, "PASS enumerated: address ");
        ub_dec(&u, seen.address);
        ub_s(&u, ", configured, ");
        ub_s(&u, full ? "full speed" : "high speed");
        ub_s(&u, "; stopped and put back as found");
    }
    plog(u.b);
    return 1;
}

int drv_usb_probe(int level, unsigned *wait_ms)
{
    uint64_t now = sb_ms();
    *wait_ms = 0;
    if (P.level != level + 1) {
        if (P.owned)
            drv_usb_abort();
        P.level = level + 1;
        P.phase = 0;
    }
    if (level == 0) {
        probe_reset();
        return probe_level0();
    }
    if (level != 1) {
        probe_reset();
        plog("PASS level 2: nothing attended to do (USB has no step a person would notice)");
        return 1;
    }
    if (P.phase == 0) {
        uint8_t bc = 0;
        struct surv_usb sv;
        if (L.enabled && L.st != USB_OFF) {
            probe_reset();
            plog("FAIL the USB service already runs (the drvprobe image keeps it off)");
            return 1;
        }
        if (!sb_usbc_read(R_BC_STATUS, &bc)) {
            probe_reset();
            plog("FAIL the MAX77705 USB-C block did not answer on I2C");
            return 1;
        }
        surv_usb_status(&sv);
        int want = usb_decide(!!(bc & BC_VBUSDET), (int)(bc & BC_CHGTYP), 10000U, sv.state == SURV_USB_ON);
        if (want != USB_UP) {
            probe_reset();
            plog(want == USB_CHARGER ? "PASS level 1 skipped: a wall charger (DCP), not a computer"
                                     : "PASS level 1 skipped: no USB cable from a computer (VBUS low)");
            return 1;
        }
        P.owned = 1;
        P.t0 = P.t_rd = now;
        L.probe = 1;
        L.vbus = 1;
        L.lows = 0;
        L.t_in = now;
        L.reroute_i = 0;
        L.resets = 0;
        L.retry_ms = 5000U;
        plog("level 1: D+/D- to the AP, the PHY and the core up as the CDC-ACM console for at most 10 s");
        link_up(now);
        if (L.st != USB_UP)
            return probe_finish(L.why && L.why[0] ? L.why : "the USB start failed");
        P.phase = 1;
        *wait_ms = 2;
        return 0;
    }
    /* phase 1: running */
    if (now - P.t0 >= 14000U)
        return probe_finish("did not finish in 14 s");
    if (now - P.t_rd >= 500U) {
        uint8_t bc = 0;
        P.t_rd = now;
        if (sb_usbc_read(R_BC_STATUS, &bc) && !(bc & BC_VBUSDET) && ++L.lows >= 2)
            return probe_finish("the cable was pulled during the probe");
        if (bc & BC_VBUSDET)
            L.lows = 0;
    }
    if (!L.configured && L.reroute_i < 3 && now - L.t_in >= reroute_ms(L.reroute_i)) {
        ++L.reroute_i;
        if (!R.pending)
            route_write(now);
    }
    route_step(now);
    service(now);
    if ((L.configured && now - L.t_cfg >= 2000U) || now - P.t0 >= 10000U)
        return probe_finish(0);
    *wait_ms = 2;
    return 0;
}

void drv_usb_abort(void)
{
    if (P.owned) {
        if (L.core)
            usbd_stop();
        L.core = 0;
        phy_restore();
        plog("aborted: the controller stopped and the PHY put back as found");
        probe_release();
    }
    probe_reset();
}
