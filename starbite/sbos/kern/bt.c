/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: Bluetooth, the BCM4361's Bluetooth half (docs/drivers/BLUETOOTH.md).
 *
 * Wiring (device/dtb/starlte.dts: bluetooth { compatible = "samsung,bcm43xx"; gpios = <0x4c 7 0  0x7e 0 0  0x81 3 15>;
 * pinctrl-0 = <0x198 0x199> }):
 *   BT_REG_ON     pin 7 (P0_7) of the PCAL6524 expander (phandle 0x4c, 0x22 on hsi2c@10930000): pcal6524.c
 *   BT_WAKE       gpg2-0 (phandle 0x7e; PERIC0 GPIO 0x104300C0), output ("bt-btwake", pinctrl 0x199): the host's
 *                 "stay awake"; high while the chip runs
 *   BT_HOST_WAKE  gpa2-3 (phandle 0x81; ALIVE GPIO 0x14050060), input ("bt-hostwake", pinctrl 0x198): read only
 *   UART          uart@10840000 (UART_BT, "uart1"), pins gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS (ALIVE 0x140500A0,
 *                 function 2, pull-up: "uart1-bus"); gpq0-4/5 are the PMIC's SPEEDY bus and are never written.
 *                 Clocks CMU_PERIC1 +0x2034/+0x201C/+0x20C4/+0x20C8 (gates), +0x3028 (Q-channel), +0x1814 (divider):
 *                 read only unless bt_allow_clock_gates(1). bt_uart.c.
 *   WLAN_REG_ON   gpg0-5 (Wi-Fi's, starbite/wifi): read only here, never written. The two halves have separate power
 *                 lines; nothing of Wi-Fi's (PCIe, its pins, its clocks) is touched.
 *   32 kHz        the chip's sleep clock comes from the S2MPS18; its RTCBUF register (0x13) is read and logged only.
 * Bring-up (Linux btbcm.c / hci_bcm.c / Samsung bcm43xx.c via an earlier port's notes, facts only; an earlier port's XNU driver runs on
 * this S9 with a paired device staying connected): BT_WAKE high; BT_REG_ON low 10 ms, high, 100 ms; 115200 8N1
 * RTS/CTS; HCI_Reset; Read_Local_Version (the ROM); Download_Minidriver 0xFC2E, 50 ms; the .hcd's records one by one,
 * each answered before the next; Launch_RAM; 250 ms; HCI_Reset; Read_Local_Version (the patch); Update_UART_Baud_Rate
 * 0xFC18 (answered at the old rate) then the UART to the new rate (3 Mbaud from a 200 MHz clock; an earlier port used 4 Mbaud);
 * Read_Local_Version at the new rate (if silent: the chip off, on, patched again, kept at 115200).
 * The patch: VENDOR firmware/bcm4361B2_semco.hcd (S9 #1's module is SEMCO: Wi-Fi read OTP a1 33) or _murata, also
 * etc/bluetooth/, then the card's /STARBITE/FW/. Read and checked whole before the chip is powered. Never in git.
 * Address: the controller's own (Read_BD_ADDR), never EFS; never logged; LE scans use a random static address.
 * Seen on a phone (2026-11-12 18:26 and 22:32 probes): the bring-up, the patch, 3 Mbaud with RTS/CTS, LE scan and
 * inquiry (11 devices). Keyboards (bt_pair.c, bt_l2cap.c, bt_hid.c) ride on this file's UART and event loop: events
 * the scan does not take go to bt_pair.c, ACL packets too, and its H4 bytes go out here; not seen on a phone yet. */
#include "bt.h"
#include "bt_uart.h"
#include "bt_pair.h"

#define ALIVE 0x14050000ULL
#define GPQ0 (ALIVE + 0xA0)
#define GPA2 (ALIVE + 0x60)
#define PERIC0 0x10430000ULL
#define GPG2 (PERIC0 + 0xC0)
#define GPG0 (PERIC0 + 0x80)
#define CMU1 0x10800000ULL
#define BT_EN_PIN 7U
#define WAKE_PIN 0U
#define HOSTWAKE_PIN 3U
#define WLAN_PIN 5U
static const uint32_t ugate[4] = { 0x2034, 0x201C, 0x20C4, 0x20C8 };
#define UQCH 0x3028
#define UDIV 0x1814
#define UMUX 0x0120
#define I2C_GATE_LO 0x2050
#define GATE_MANUAL (1U << 20)
#define GATE_ON (1U << 21)

#define FW_CAP (256U * 1024U)
#define SLICE_US 20000U
#define CMD_MS 2000U

/* ---- the HAL, with every write checked ---- */
static struct bt_hal G;
static void (*user_wr)(uint64_t, uint32_t);
static int hal_ok, allow_gates, semco_first = 1;

static int allowed(uint64_t pa)
{
    if ((pa >= BTU_BASE && pa < BTU_BASE + 0x100) || (pa >= PCAL_BUS && pa < PCAL_BUS + 0x100))
        return 1;
    if (pa == GPQ0 || pa == GPQ0 + 8 || pa == GPG2 || pa == GPG2 + 4)
        return 1;
    if (pa == 0x1081103CULL || pa == 0x10830040ULL || pa == 0x10830048ULL || pa == 0x108300C0ULL ||
        pa == 0x108300C4ULL)
        return 1;                           /* pcal6524.c's: USI10 mode, gpp6, gpg3 */
    if (allow_gates) {
        for (unsigned i = 0; i < 4; i++)
            if (pa == CMU1 + ugate[i])
                return 1;
        if (pa == CMU1 + UQCH || pa == CMU1 + 0x304C || pa == CMU1 + 0x2050 || pa == CMU1 + 0x20A8 ||
            pa == CMU1 + 0x210C || pa == CMU1 + 0x2110)
            return 1;
    }
    return 0;
}

static unsigned refused_writes;
static void guard_wr(uint64_t pa, uint32_t v)
{
    if (!allowed(pa) || (pa & 3)) {
        if (!refused_writes++) {
            struct btl l;
            btl_init(&l);
            btl_s(&l, "bt: refused a write outside Bluetooth's blocks at 0x");
            btl_x(&l, pa, 8);
            G.io.log(l.b);
        }
        return;
    }
    user_wr(pa, v);
}

static void btp_glue_init(void);
void bt_hal_set(const struct bt_hal *h)
{
    G = *h;
    user_wr = h->io.wr32;
    G.io.wr32 = guard_wr;
    hal_ok = 1;
    btp_glue_init();
}

void bt_set_module(int semco)
{
    semco_first = semco != 0;
}

void bt_allow_clock_gates(int on)
{
    allow_gates = on != 0;
}

static void say(const char *s)
{
    G.io.log(s);
}

static uint64_t now(void)
{
    return G.now_us();
}

/* ---- state ---- */
enum {
    P_IDLE, P_FW, P_LOOK, P_EXP, P_UART, P_PWR0, P_PWR1, P_PWR2, P_RESET1, P_VER1, P_MINI, P_REC, P_POST, P_RESET2,
    P_VER2, P_BAUD, P_BAUD2, P_VER3, P_ADDR, P_EVMASK, P_INQMODE, P_RANDADDR, P_READY, P_LE_PARAMS, P_LE_ON,
    P_LE_RUN, P_LE_OFF, P_INQ, P_INQ_RUN, P_INQ_CANCEL, P_SCAN_END,
};
static int st = BT_OFF, ph = P_IDLE;
static const char *why_s = "Bluetooth is off";
static uint64_t wake;
static struct bt_uart U;
static struct bthci_rx rx;
static struct bthci_devs devs;
static struct bt_info inf;
static unsigned sorted[BT_MAX_DEV], nsorted;

static uint8_t *fw;
static uint32_t fw_len, fw_off, fw_sent;
static struct bthci_hcd fwi;
static struct bthci_rec cur;
static int fw_loaded;

/* the command in flight */
static uint16_t c_op;
static int c_st;                            /* 1 waiting, 2 done, -1 refused, -2 no answer, -3 not sent */
static uint8_t c_status, c_ret[255], c_rl;
static uint64_t c_dead;
static int c_accept_status, issued;

/* what this run changed */
static int look_saved, uart_saved, exp_open, chip_powered, wake_set, gates_set, restored = 1;
static uint32_t gpq0_con, gpq0_pud, gpg2_con, gpg2_dat, gpg0_con0, gpg0_dat0, gate_was[4], qch_was;
static int no_fast, rand_ok;
static uint32_t target_baud;
static unsigned scan_le_ms, scan_inq_ms;
static uint64_t scan_end;
static unsigned other_events;

static void go(int p)
{
    ph = p;
    issued = 0;
}

static void after_ms(uint32_t ms)
{
    wake = now() + (uint64_t)ms * 1000U;
}

static void logv(const char *what, const struct bthci_version *v)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: ");
    btl_s(&l, what);
    btl_s(&l, ": HCI ");
    btl_u(&l, v->hci_ver);
    btl_s(&l, " rev ");
    btl_x(&l, v->hci_rev, 4);
    btl_s(&l, ", LMP ");
    btl_u(&l, v->lmp_ver);
    btl_s(&l, " subversion ");
    btl_x(&l, v->lmp_subver, 4);
    btl_s(&l, " (");
    btl_u(&l, v->lmp_subver >> 13);
    btl_s(&l, ".");
    btl_u(&l, (v->lmp_subver >> 8) & 0x1f);
    btl_s(&l, ".");
    btl_u(&l, v->lmp_subver & 0xff);
    btl_s(&l, " build ");
    btl_u(&l, v->hci_rev & 0xfff);
    btl_s(&l, v->manufacturer == BCM_MANUFACTURER ? "), Broadcom" : "), NOT Broadcom's 000f: ");
    if (v->manufacturer != BCM_MANUFACTURER)
        btl_x(&l, v->manufacturer, 4);
    say(l.b);
}

/* ---- events in ---- */

static void on_event(const uint8_t *ev)
{
    uint16_t op;
    uint8_t s, rl;
    const uint8_t *r;
    if (bthci_cmd_complete(ev, &op, &s, &r, &rl)) {
        if (c_st == 1 && op == c_op) {
            c_status = s;
            if (s == 0 || s == 0xFF) {
                bthci_copy(c_ret, r, rl);
                c_rl = rl;
                c_st = 2;
            } else {
                c_st = -1;
            }
        } else {
            (void)btp_event(ev);            /* the keyboard's commands */
        }
        return;
    }
    if (bthci_cmd_status(ev, &op, &s)) {
        if (c_st == 1 && op == c_op) {
            c_status = s;
            if (s)
                c_st = -1;
            else if (c_accept_status)
                c_st = 2;
        } else {
            (void)btp_event(ev);
        }
        return;
    }
    if (bthci_scan_event(&devs, ev))
        return;
    if (btp_event(ev))
        return;
    if (ev[0] == BTEV_HW_ERROR) {
        struct btl l;
        btl_init(&l);
        btl_s(&l, "bt: the chip reported a hardware error, code ");
        btl_x(&l, ev[1] ? ev[2] : 0, 2);
        say(l.b);
        return;
    }
    if (other_events++ < 4) {
        struct btl l;
        btl_init(&l);
        btl_s(&l, "bt: an event passed over: ");
        btl_x(&l, ev[0], 2);
        btl_s(&l, ", ");
        btl_u(&l, ev[1]);
        btl_s(&l, " bytes");
        say(l.b);
    }
}

static void pump(void)
{
    if (!U.up)
        return;
    for (unsigned i = 0; i < 2048; i++) {
        int c = bt_uart_getc(&U);
        if (c < 0)
            break;
        int k = bthci_rx_byte(&rx, (uint8_t)c);
        if (k == BTHCI_RX_EVENT)
            on_event(rx.ev);
        else if (k == BTHCI_RX_ACL)
            btp_acl(rx.acl, 4U + (unsigned)(rx.acl[2] | rx.acl[3] << 8));
    }
}

/* a command: 0 waiting (sent on the first call of a phase), 1 answered with status 0, -1 refused / silent / unsent */
static int do_cmd(uint16_t op, const uint8_t *p, uint8_t n, uint32_t ms, int accept_status)
{
    if (!issued) {
        if (btp_cmd_busy())
            return 0;                       /* the keyboard's command is in flight: ours after it */
        uint8_t pkt[4 + 255];
        unsigned k = bthci_cmd(pkt, sizeof(pkt), op, p, n);
        issued = 1;
        c_op = op;
        c_st = 1;
        c_rl = 0;
        c_status = 0;
        c_accept_status = accept_status;
        c_dead = now() + (uint64_t)ms * 1000U;
        if (!k || !bt_uart_write(&U, pkt, k, 50000)) {
            c_st = -3;
            return -1;
        }
        return 0;
    }
    if (c_st == 1 && now() >= c_dead)
        c_st = -2;
    if (c_st == 1)
        return 0;
    return c_st == 2 ? 1 : -1;
}

static void log_cmd_fail(const char *what)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: ");
    btl_s(&l, what);
    btl_s(&l, " (");
    btl_x(&l, c_op, 4);
    btl_s(&l, c_st == -1 ? "): refused, status " : c_st == -2 ? "): no answer in time" : "): could not be sent (CTS ");
    if (c_st == -1)
        btl_x(&l, c_status, 2);
    if (c_st == -3)
        btl_s(&l, bt_uart_cts(&U) ? "asserted)" : "not asserted)");
    btl_s(&l, "; UART receive errors ");
    btl_u(&l, U.rx_err);
    btl_s(&l, ", bytes not a packet ");
    btl_u(&l, rx.junk);
    say(l.b);
}

/* ---- off, and everything back ---- */

static uint32_t rd(uint64_t pa)
{
    return G.io.rd32(pa);
}

static void shutdown(void)
{
    int same = 1;
    if (chip_powered && exp_open) {
        if (pcal_pin_restore(BT_EN_PIN)) {
            say("bt: BT_REG_ON could not be brought back: the chip may stay powered until the phone restarts");
            same = 0;
        } else {
            say("bt: the chip off (BT_REG_ON low, then as found)");
        }
    }
    chip_powered = 0;
    if (wake_set) {
        uint32_t c = rd(GPG2), d = rd(GPG2 + 4);
        G.io.wr32(GPG2 + 4, (d & ~(1U << WAKE_PIN)) | (gpg2_dat & 1U << WAKE_PIN));
        G.io.wr32(GPG2, (c & ~(0xfU << (4 * WAKE_PIN))) | (gpg2_con & 0xfU << (4 * WAKE_PIN)));
        if (((rd(GPG2) ^ gpg2_con) & 0xfU) || ((rd(GPG2 + 4) ^ gpg2_dat) & 1U))
            same = 0, say("bt: BT_WAKE (gpg2-0) is not as found");
        wake_set = 0;
    }
    if (uart_saved) {
        bt_uart_restore(&U);
        if (!bt_uart_same(&U))
            same = 0, say("bt: UART_BT's registers are not as found");
        uart_saved = 0;
    }
    U.up = 0;
    if (look_saved) {
        uint32_t c = rd(GPQ0), p = rd(GPQ0 + 8);
        G.io.wr32(GPQ0, (c & ~0xffffU) | (gpq0_con & 0xffffU));
        G.io.wr32(GPQ0 + 8, (p & ~0xffffU) | (gpq0_pud & 0xffffU));
        if (((rd(GPQ0) ^ gpq0_con) & 0xffffU) || ((rd(GPQ0 + 8) ^ gpq0_pud) & 0xffffU))
            same = 0, say("bt: the UART pins gpq0-0..3 are not as found");
        if (((rd(GPG0) ^ gpg0_con0) & 0xfU << (4 * WLAN_PIN)) || ((rd(GPG0 + 4) ^ gpg0_dat0) & 1U << WLAN_PIN))
            say("bt: WLAN_REG_ON (gpg0-5) changed during the run (not by this driver: Wi-Fi's own)");
        look_saved = 0;
    }
    if (exp_open) {
        if (!pcal_close())
            same = 0;
        exp_open = 0;
    }
    if (gates_set) {
        G.io.wr32(CMU1 + UQCH, qch_was);
        for (unsigned i = 0; i < 4; i++)
            G.io.wr32(CMU1 + ugate[i], gate_was[i]);
        for (unsigned i = 0; i < 4; i++)
            if (rd(CMU1 + ugate[i]) != gate_was[i])
                same = 0, say("bt: a UART_BT clock gate is not as found");
        gates_set = 0;
    }
    restored = same;
    inf.rx_errors = U.rx_err;
    inf.junk = rx.junk;
    btp_chip_off();                         /* a keyboard link is gone with the chip */
}

static void fail(const char *w)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: stopped: ");
    btl_s(&l, w);
    say(l.b);
    why_s = w;
    shutdown();
    st = BT_FAILED;
    go(P_IDLE);
}

void bt_off(void)
{
    if (!hal_ok)
        return;
    int was = st;
    shutdown();
    if (was != BT_OFF && was != BT_FAILED)
        say(restored ? "bt: off, everything back as found" : "bt: off, NOT everything back as found (see above)");
    st = BT_OFF;
    why_s = "Bluetooth is off";
    go(P_IDLE);
}

int bt_restored(void)
{
    return restored;
}

/* ---- the firmware ---- */

void bt_fw_forget(void)
{
    fw_loaded = 0;
    fw_len = 0;
    inf.fw[0] = 0;
}

static int load_fw(void)
{
    static const char *const mods[2] = { "semco", "murata" };
    static const char *const vdirs[2] = { "firmware/", "etc/bluetooth/" };
    char path[64];
    const char *last = "The Bluetooth firmware is missing (VENDOR firmware/bcm4361B2_<module>.hcd or the card's "
                       "/STARBITE/FW/)";
    if (fw_loaded)
        return 1;
    if (!fw && G.alloc)
        fw = G.alloc(FW_CAP);
    if (!fw) {
        why_s = "No memory for the Bluetooth firmware";
        return 0;
    }
    for (unsigned src = 0; src < 2; src++)
        for (unsigned m = 0; m < 2; m++)
            for (unsigned d = 0; d < (src ? 1U : 2U); d++) {
                const char *mod = mods[semco_first ? m : 1 - m];
                struct btl p;
                btl_init(&p);
                btl_s(&p, src ? "/STARBITE/FW/" : vdirs[d]);
                btl_s(&p, "bcm4361B2_");
                btl_s(&p, mod);
                btl_s(&p, ".hcd");
                bthci_copy(path, p.b, p.n + 1 < sizeof(path) ? p.n + 1 : sizeof(path));
                path[sizeof(path) - 1] = 0;
                long (*rdf)(const char *, void *, uint32_t) = src ? G.card_read : G.vendor_read;
                if (!rdf)
                    continue;
                long n = rdf(path, fw, FW_CAP);
                if (n <= 0)
                    continue;
                int e = bthci_hcd_check(fw, (uint32_t)n, &fwi);
                struct btl l;
                btl_init(&l);
                btl_s(&l, "bt: firmware ");
                btl_s(&l, src ? "card " : "VENDOR ");
                btl_s(&l, path);
                btl_s(&l, ": ");
                btl_u(&l, (uint64_t)n);
                btl_s(&l, " bytes");
                if (e != BTHCI_HCD_OK) {
                    btl_s(&l, ", REFUSED: ");
                    btl_s(&l, bthci_hcd_why(e));
                    say(l.b);
                    last = "The Bluetooth firmware file is damaged (refused before sending)";
                    continue;
                }
                btl_s(&l, ", ");
                btl_u(&l, fwi.records);
                btl_s(&l, " records (");
                btl_u(&l, fwi.writes);
                btl_s(&l, " Write_RAM, ");
                btl_u(&l, fwi.bytes);
                btl_s(&l, " bytes), launch at ");
                btl_x(&l, fwi.launch, 8);
                btl_s(&l, "; \"");
                btl_s(&l, fwi.name[0] ? fwi.name : "no description");
                btl_s(&l, "\"");
                say(l.b);
                fw_len = (uint32_t)n;
                bthci_copy(inf.fw, path, sizeof(inf.fw));
                inf.fw[sizeof(inf.fw) - 1] = 0;
                bthci_copy(inf.fw_name, fwi.name, sizeof(inf.fw_name));
                inf.fw_records = fwi.records;
                fw_loaded = 1;
                return 1;
            }
    why_s = last;
    return 0;
}

/* ---- the look (level 0, and the start of every run) ---- */

static uint32_t pr(uint64_t pa, int *ok)
{
    uint32_t v = 0;
    if (!G.io.probe32(pa, &v)) {
        *ok = 0;
        return 0xdeadbeefU;
    }
    return v;
}

static int look(int keep)
{
    int ok = 1, closed = 0;
    uint32_t qc = pr(GPQ0, &ok), qp = pr(GPQ0 + 8, &ok), qd = pr(GPQ0 + 4, &ok);
    uint32_t wc = pr(GPG2, &ok), wd = pr(GPG2 + 4, &ok), hc = pr(GPA2, &ok), hd = pr(GPA2 + 4, &ok);
    uint32_t lc = pr(GPG0, &ok), ld = pr(GPG0 + 4, &ok);
    uint32_t g[4], q = pr(CMU1 + UQCH, &ok), dv = pr(CMU1 + UDIV, &ok), mx = pr(CMU1 + UMUX, &ok);
    for (unsigned i = 0; i < 4; i++) {
        g[i] = pr(CMU1 + ugate[i], &ok);
        if ((g[i] & (GATE_MANUAL | GATE_ON)) == GATE_MANUAL)
            closed = 1;
    }
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: pins: gpq0 con ");
    btl_x(&l, qc & 0xffffU, 4);
    btl_s(&l, " pud ");
    btl_x(&l, qp & 0xffffU, 4);
    btl_s(&l, " dat ");
    btl_x(&l, qd & 0xfU, 1);
    btl_s(&l, " (UART wants 2222 / 3333); BT_WAKE gpg2-0 fn ");
    btl_x(&l, wc & 0xfU, 1);
    btl_s(&l, " lvl ");
    btl_u(&l, wd & 1U);
    btl_s(&l, "; HOST_WAKE gpa2-3 fn ");
    btl_x(&l, (hc >> 12) & 0xfU, 1);
    btl_s(&l, " lvl ");
    btl_u(&l, (hd >> HOSTWAKE_PIN) & 1U);
    btl_s(&l, "; WLAN_REG_ON gpg0-5 fn ");
    btl_x(&l, (lc >> 20) & 0xfU, 1);
    btl_s(&l, " lvl ");
    btl_u(&l, (ld >> WLAN_PIN) & 1U);
    btl_s(&l, ((ld >> WLAN_PIN) & 1U) ? " (Wi-Fi powered)" : " (Wi-Fi off)");
    say(l.b);
    btl_init(&l);
    btl_s(&l, "bt: CMU_PERIC1 UART_BT gates ");
    for (unsigned i = 0; i < 4; i++) {
        btl_x(&l, g[i], 8);
        btl_s(&l, i < 3 ? "/" : "");
    }
    btl_s(&l, " qch ");
    btl_x(&l, q, 8);
    btl_s(&l, " div ");
    btl_x(&l, dv, 8);
    btl_s(&l, " IP_USER mux ");
    btl_x(&l, mx, 8);
    btl_s(&l, (mx >> 4) & 1U ? " (CMU_TOP's clock)" : " (the 26 MHz oscillator)");
    btl_s(&l, closed ? "; a gate is FORCED OFF" : "; gates not forced off");
    say(l.b);
    if (G.pmic_read) {
        uint8_t b = 0;
        btl_init(&l);
        btl_s(&l, "bt: S2MPS18 RTCBUF (0x13, the 32 kHz buffers) ");
        if (G.pmic_read(1, 0x13, &b))
            btl_x(&l, b, 2);
        else
            btl_s(&l, "not read");
        say(l.b);
    }
    if (!ok) {
        why_s = "Bluetooth's pin or clock registers do not read";
        return 0;
    }
    int uok = 0;
    if (!closed) {
        uok = bt_uart_save(&U, &G);
        if (uok)
            bt_uart_log(&U, "as found");
    } else {
        say("bt: UART_BT's registers not read (a clock gate is forced off)");
    }
    if (keep) {
        gpq0_con = qc;
        gpq0_pud = qp;
        gpg2_con = wc;
        gpg2_dat = wd;
        gpg0_con0 = lc;
        gpg0_dat0 = ld;
        look_saved = 1;
        if (closed) {
            if (!allow_gates) {
                why_s = "UART_BT's clock gate is closed (not opened from here)";
                return 0;
            }
            for (unsigned i = 0; i < 4; i++) {
                gate_was[i] = g[i];
                G.io.wr32(CMU1 + ugate[i], g[i] | GATE_MANUAL | GATE_ON);
            }
            qch_was = q;
            G.io.wr32(CMU1 + UQCH, q | 3U);
            gates_set = 1;
            say("bt: UART_BT's gates forced on (allowed by the caller); restored when off");
            uok = bt_uart_save(&U, &G);
        }
        if (!uok) {
            why_s = "UART_BT's registers do not read";
            return 0;
        }
        uart_saved = 1;
    }
    return 1;
}

int bt_look(void)
{
    struct pcal_look pl;
    if (!hal_ok)
        return 0;
    int ok = look(0);
    if (!pcal_look(&G.io, &pl))
        ok = 0;
    pcal_look_log(&pl, "bt: ");
    say(pl.bus_ready ? "bt: the expander's bus is already set up (its ports could be read)"
                     : "bt: the expander's bus is not set up: its ports are not read at level 0 (no writes)");
    if (!load_fw())
        ok = 0;
    return ok;
}

/* ---- the bring-up ---- */

int bt_on(void)
{
    if (!hal_ok)
        return -1;
    if (st == BT_STARTING || st == BT_READY || st == BT_SCANNING)
        return 0;
    st = BT_STARTING;
    why_s = "Bluetooth is starting";
    restored = 1;
    rand_ok = 0;
    no_fast = 0;
    other_events = 0;
    refused_writes = 0;
    bthci_rx_reset(&rx);
    rx.junk = rx.skipped = 0;
    bthci_devs_clear(&devs);
    nsorted = 0;
    struct bt_info keep = inf;              /* a cached firmware (fw_loaded) keeps its name and record count */
    for (unsigned i = 0; i < sizeof(inf); i++)
        ((uint8_t *)&inf)[i] = 0;
    if (fw_loaded) {
        for (unsigned i = 0; i < sizeof(inf.fw); i++)
            inf.fw[i] = keep.fw[i];
        for (unsigned i = 0; i < sizeof(inf.fw_name); i++)
            inf.fw_name[i] = keep.fw_name[i];
        inf.fw_records = keep.fw_records;
    }
    inf.baud = 115200;
    wake = 0;
    go(P_FW);
    say("bt: on: firmware, BT_REG_ON on the PCAL6524, UART_BT at 115200, HCI_Reset, the patch, a faster rate");
    return 0;
}

int bt_scan(unsigned le_ms, unsigned inquiry_ms)
{
    if (st != BT_READY || !btp_scan_ok())
        return -1;                          /* off, starting, scanning, or a keyboard is being paired */
    bthci_devs_clear(&devs);
    nsorted = 0;
    scan_le_ms = le_ms;
    scan_inq_ms = inquiry_ms;
    st = BT_SCANNING;
    go(le_ms ? P_LE_PARAMS : P_INQ);
    return 0;
}

static void set_wake(void)
{
    uint32_t d = rd(GPG2 + 4), c = rd(GPG2);
    G.io.wr32(GPG2 + 4, d | 1U << WAKE_PIN);
    G.io.wr32(GPG2, (c & ~(0xfU << (4 * WAKE_PIN))) | 1U << (4 * WAKE_PIN));
    wake_set = 1;
}

static void run(void)
{
    int r;
    struct btl l;
    switch (ph) {
    case P_FW:
        if (!load_fw()) {
            fail(why_s);
            return;
        }
        go(P_LOOK);
        return;
    case P_LOOK:
        if (!look(1)) {
            fail(why_s);
            return;
        }
        go(P_EXP);
        return;
    case P_EXP: {
        int o = 0, d = 0, lv = 0;
        if (pcal_open(&G.io, 1U << BT_EN_PIN, allow_gates)) {
            fail(pcal_why());
            return;
        }
        exp_open = 1;
        if (pcal_pin(BT_EN_PIN, &o, &d, &lv)) {
            fail("The GPIO expander stopped answering");
            return;
        }
        btl_init(&l);
        btl_s(&l, "bt: BT_REG_ON (P0_7) as found: ");
        btl_s(&l, o ? "an output, driving " : "an input, reads ");
        btl_u(&l, o ? (unsigned)d : (unsigned)lv);
        say(l.b);
        if (o && d) {
            fail("Bluetooth is already powered by someone else (BT_REG_ON high)");
            return;
        }
        go(P_UART);
        return;
    }
    case P_UART: {
        uint32_t c = rd(GPQ0), p = rd(GPQ0 + 8);
        G.io.wr32(GPQ0 + 8, (p & ~0xffffU) | 0x3333U);
        G.io.wr32(GPQ0, (c & ~0xffffU) | 0x2222U);
        bt_uart_start(&U);
        uint32_t clk = bt_uart_measure(&U);
        if (!clk) {
            bt_uart_log(&U, "after the loopback");
            fail("UART_BT's clock does not run (its loopback sent nothing back)");
            return;
        }
        inf.uart_clk_hz = clk;
        target_baud = no_fast ? 0 : bthci_pick_baud(clk, 3000000);
        if (!bt_uart_baud(&U, 115200)) {
            fail("UART_BT's clock is too slow for 115200 baud");
            return;
        }
        {
            struct bthci_div dv;
            bthci_uart_div(clk, 115200, &dv);
            uint32_t e = dv.actual > 115200 ? dv.actual - 115200 : 115200 - dv.actual;
            if (e * 50U > 115200U) {
                fail("UART_BT's clock gives no 115200 baud within 2 %");
                return;
            }
        }
        bt_uart_afc(&U, 1);
        bt_uart_flush(&U);
        btl_init(&l);
        btl_s(&l, "bt: UART_BT out of reset, its clock measured ");
        btl_u(&l, clk / 1000);
        btl_s(&l, " kHz; 115200 8N1 with RTS/CTS; the fast rate will be ");
        btl_u(&l, target_baud ? target_baud : 115200);
        say(l.b);
        go(P_PWR0);
        return;
    }
    case P_PWR0:
        set_wake();
        chip_powered = 1;                   /* from here a failure drives BT_REG_ON back */
        if (pcal_pin_out(BT_EN_PIN, 0)) {
            fail("The GPIO expander did not take BT_REG_ON");
            return;
        }
        say("bt: BT_WAKE high; BT_REG_ON an output, low; 10 ms");
        after_ms(10);
        go(P_PWR1);
        return;
    case P_PWR1:
        if (pcal_pin_out(BT_EN_PIN, 1)) {
            fail("The GPIO expander did not take BT_REG_ON");
            return;
        }
        say("bt: BT_REG_ON high: the chip powered; 100 ms");
        after_ms(100);
        go(P_PWR2);
        return;
    case P_PWR2: {
        unsigned junk = 0;
        while (bt_uart_getc(&U) >= 0 && junk < 1024)
            junk++;
        bthci_rx_reset(&rx);
        btl_init(&l);
        btl_s(&l, "bt: CTS ");
        btl_s(&l, bt_uart_cts(&U) ? "asserted" : "NOT asserted");
        btl_s(&l, ", HOST_WAKE ");
        btl_u(&l, (rd(GPA2 + 4) >> HOSTWAKE_PIN) & 1U);
        btl_s(&l, ", bytes from the chip on its own ");
        btl_u(&l, junk);
        say(l.b);
        go(P_RESET1);
        return;
    }
    case P_RESET1:
    case P_RESET2:
        r = do_cmd(BTHCI_RESET, 0, 0, CMD_MS, 0);
        if (r < 0) {
            log_cmd_fail("HCI_Reset");
            fail(ph == P_RESET1 ? "The Bluetooth chip does not answer (HCI_Reset after power-on)"
                                : "The Bluetooth chip does not answer after its patch");
        } else if (r) {
            go(ph == P_RESET1 ? P_VER1 : P_VER2);
        }
        return;
    case P_VER1:
    case P_VER2:
    case P_VER3: {
        struct bthci_version v;
        r = do_cmd(BTHCI_READ_LOCAL_VERSION, 0, 0, ph == P_VER3 ? 500U : CMD_MS, 0);
        if (!r)
            return;
        if (r > 0 && !bthci_version(c_ret, c_rl, &v))
            r = -1;
        if (r < 0 && ph == P_VER3) {
            btl_init(&l);
            btl_s(&l, "bt: no answer at ");
            btl_u(&l, target_baud);
            btl_s(&l, " baud: the chip off, on again, patched again, kept at 115200");
            say(l.b);
            no_fast = 1;
            target_baud = 0;
            (void)pcal_pin_out(BT_EN_PIN, 0);
            (void)bt_uart_baud(&U, 115200);
            inf.baud = 115200;
            bt_uart_flush(&U);
            fw_off = 0;
            after_ms(200);
            go(P_PWR0);
            return;
        }
        if (r < 0) {
            log_cmd_fail("Read_Local_Version");
            fail("The Bluetooth chip does not give its version");
            return;
        }
        inf.now = v;
        if (ph == P_VER1) {
            inf.rom = v;
            logv("the ROM", &v);
            go(P_MINI);
        } else if (ph == P_VER2) {
            inf.patched = v;
            logv("patched", &v);
            if (v.lmp_subver == inf.rom.lmp_subver && v.hci_rev == inf.rom.hci_rev)
                say("bt: the version did not change: the patch may not have taken");
            go(P_BAUD);
        } else {
            btl_init(&l);
            btl_s(&l, "bt: at ");
            btl_u(&l, inf.baud);
            btl_s(&l, " baud with RTS/CTS the chip answers");
            say(l.b);
            go(P_ADDR);
        }
        return;
    }
    case P_MINI:
        r = do_cmd(BCM_DOWNLOAD_MINIDRIVER, 0, 0, CMD_MS, 0);
        if (r < 0) {
            log_cmd_fail("Download_Minidriver");
            fail("The Bluetooth chip refused the patch download");
        } else if (r) {
            fw_off = 0;
            fw_sent = 0;
            say("bt: Download_Minidriver taken; 50 ms; the records one by one");
            after_ms(50);
            go(P_REC);
        }
        return;
    case P_REC: {
        if (!issued) {
            int e = bthci_hcd_next(fw, fw_len, &fw_off, &cur);
            if (e != 1) {
                fail("The Bluetooth firmware ended without Launch_RAM");
                return;
            }
        }
        int last = cur.op == BCM_LAUNCH_RAM;
        r = do_cmd(cur.op, cur.p, cur.n, last ? 1000U : CMD_MS, 0);
        if (!r)
            return;
        if (r < 0 && !last) {
            btl_init(&l);
            btl_s(&l, "bt: patch record ");
            btl_u(&l, fw_sent + 1);
            btl_s(&l, " of ");
            btl_u(&l, fwi.records);
            btl_s(&l, " failed");
            say(l.b);
            log_cmd_fail("a patch record");
            fail("The Bluetooth chip did not take its patch");
            return;
        }
        fw_sent++;
        issued = 0;
        if (last) {
            btl_init(&l);
            btl_s(&l, "bt: ");
            btl_u(&l, fw_sent - 1);
            btl_s(&l, r > 0 ? " records sent; Launch_RAM taken; 250 ms" :
                              " records sent; Launch_RAM unanswered (the chip may have restarted at once); 250 ms");
            say(l.b);
            after_ms(250);
            go(P_POST);
        } else if (fw_sent % 100 == 0) {
            btl_init(&l);
            btl_s(&l, "bt: patch: ");
            btl_u(&l, fw_sent);
            btl_s(&l, " of ");
            btl_u(&l, fwi.records);
            say(l.b);
        }
        return;
    }
    case P_POST:
        bt_uart_flush(&U);
        bthci_rx_reset(&rx);
        go(P_RESET2);
        return;
    case P_BAUD: {
        if (!target_baud || target_baud == 115200) {
            say("bt: staying at 115200 baud");
            go(P_ADDR);
            return;
        }
        uint8_t p[6] = { 0, 0, (uint8_t)target_baud, (uint8_t)(target_baud >> 8), (uint8_t)(target_baud >> 16),
                         (uint8_t)(target_baud >> 24) };
        r = do_cmd(BCM_UPDATE_BAUD, p, sizeof(p), CMD_MS, 0);
        if (r < 0) {
            log_cmd_fail("Update_UART_Baud_Rate");
            say("bt: staying at 115200 baud");
            go(P_ADDR);
        } else if (r) {
            (void)bt_uart_baud(&U, target_baud);
            inf.baud = target_baud;
            after_ms(10);
            go(P_BAUD2);
        }
        return;
    }
    case P_BAUD2:
        bt_uart_flush(&U);
        bthci_rx_reset(&rx);
        go(P_VER3);
        return;
    case P_ADDR:
        r = do_cmd(BTHCI_READ_BD_ADDR, 0, 0, CMD_MS, 0);
        if (!r)
            return;
        if (r > 0 && c_rl >= 6) {
            int zero = 1;
            bthci_copy(inf.addr, c_ret, 6);
            for (unsigned i = 0; i < 6; i++)
                if (inf.addr[i])
                    zero = 0;
            inf.addr_read = !zero;
            inf.addr_default = inf.addr[5] == 0x43 && inf.addr[4] == 0x61 && inf.addr[3] == 0xB2;
            say(zero ? "bt: address read: none (all zero)"
                     : inf.addr_default ? "bt: address read: Broadcom's default (the same on every unprogrammed chip; "
                                          "not unique)"
                                        : "bt: address read: the chip's own");
        } else {
            log_cmd_fail("Read_BD_ADDR");
        }
        go(P_EVMASK);
        return;
    case P_EVMASK: {
        static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };   /* + LE Meta */
        r = do_cmd(BTHCI_SET_EVENT_MASK, mask, sizeof(mask), CMD_MS, 0);
        if (r < 0)
            log_cmd_fail("Set_Event_Mask");
        if (r)
            go(P_INQMODE);
        return;
    }
    case P_INQMODE: {
        static const uint8_t mode = 2;      /* results with RSSI or EIR */
        r = do_cmd(BTHCI_WRITE_INQUIRY_MODE, &mode, 1, CMD_MS, 0);
        if (r < 0)
            log_cmd_fail("Write_Inquiry_Mode");
        if (r)
            go(P_RANDADDR);
        return;
    }
    case P_RANDADDR: {
        static uint8_t a[6];
        if (!issued) {
            uint64_t x = now() * 0x9E3779B97F4A7C15ULL ^ (uint64_t)U.rx_err << 32 ^ rx.junk;
            for (unsigned i = 0; i < 6; i++) {
                x ^= x >> 29;
                x *= 0xBF58476D1CE4E5B9ULL;
                a[i] = (uint8_t)(x >> 32);
            }
            a[5] |= 0xC0;                   /* a random static address */
        }
        r = do_cmd(BTHCI_LE_SET_RANDOM_ADDR, a, 6, CMD_MS, 0);
        if (r < 0)
            log_cmd_fail("LE_Set_Random_Address (LE scans will be passive)");
        if (r) {
            rand_ok = r > 0;
            go(P_READY);
        }
        return;
    }
    case P_READY:
        st = BT_READY;
        why_s = "Bluetooth is on";
        btl_init(&l);
        btl_s(&l, "bt: ready: patched, ");
        btl_u(&l, inf.baud);
        btl_s(&l, " baud");
        say(l.b);
        go(P_IDLE);
        return;
    case P_LE_PARAMS: {
        /* active (with a random static address) or passive, 60 ms interval, 30 ms window, no filter */
        uint8_t p[7] = { (uint8_t)(rand_ok ? 1 : 0), 0x60, 0x00, 0x30, 0x00, (uint8_t)(rand_ok ? 1 : 0), 0 };
        r = do_cmd(BTHCI_LE_SET_SCAN_PARAMS, p, sizeof(p), CMD_MS, 0);
        if (r < 0) {
            log_cmd_fail("LE_Set_Scan_Parameters");
            go(P_INQ);
        } else if (r) {
            go(P_LE_ON);
        }
        return;
    }
    case P_LE_ON: {
        static const uint8_t on[2] = { 1, 1 };
        r = do_cmd(BTHCI_LE_SET_SCAN_ENABLE, on, 2, CMD_MS, 0);
        if (r < 0) {
            log_cmd_fail("LE_Set_Scan_Enable");
            go(P_INQ);
        } else if (r) {
            scan_end = now() + (uint64_t)scan_le_ms * 1000U;
            go(P_LE_RUN);
        }
        return;
    }
    case P_LE_RUN:
        if (now() >= scan_end)
            go(P_LE_OFF);
        else
            wake = now() + 10000U < scan_end ? now() + 10000U : scan_end;
        return;
    case P_LE_OFF: {
        static const uint8_t off[2] = { 0, 0 };
        r = do_cmd(BTHCI_LE_SET_SCAN_ENABLE, off, 2, CMD_MS, 0);
        if (r < 0)
            log_cmd_fail("LE_Set_Scan_Enable (off)");
        if (r) {
            btl_init(&l);
            btl_s(&l, "bt: LE scan: ");
            btl_u(&l, devs.n);
            btl_s(&l, " devices, ");
            btl_u(&l, devs.reports);
            btl_s(&l, " reports");
            say(l.b);
            go(P_INQ);
        }
        return;
    }
    case P_INQ: {
        if (!scan_inq_ms) {
            go(P_SCAN_END);
            return;
        }
        unsigned len = (scan_inq_ms + 1279) / 1280;
        if (len < 1)
            len = 1;
        if (len > 48)
            len = 48;
        uint8_t p[5] = { 0x33, 0x8b, 0x9e, (uint8_t)len, 0 };     /* the general inquiry access code */
        r = do_cmd(BTHCI_INQUIRY, p, sizeof(p), CMD_MS, 1);
        if (r < 0) {
            log_cmd_fail("Inquiry");
            go(P_SCAN_END);
        } else if (r) {
            devs.inquiry_done = 0;
            scan_end = now() + (uint64_t)len * 1280000U + 2000000U;
            go(P_INQ_RUN);
        }
        return;
    }
    case P_INQ_RUN:
        if (devs.inquiry_done)
            go(P_SCAN_END);
        else if (now() >= scan_end)
            go(P_INQ_CANCEL);
        else
            wake = now() + 10000U;
        return;
    case P_INQ_CANCEL:
        r = do_cmd(BTHCI_INQUIRY_CANCEL, 0, 0, CMD_MS, 0);
        if (r < 0)
            log_cmd_fail("Inquiry_Cancel");
        if (r) {
            say("bt: the inquiry did not end by itself: cancelled");
            go(P_SCAN_END);
        }
        return;
    case P_SCAN_END: {
        unsigned le = 0;
        for (unsigned i = 0; i < devs.n; i++)
            le += devs.d[i].le;
        nsorted = bthci_devs_sorted(&devs, sorted, BT_MAX_DEV);
        btl_init(&l);
        btl_s(&l, "bt: scan done: ");
        btl_u(&l, devs.n);
        btl_s(&l, " devices (");
        btl_u(&l, le);
        btl_s(&l, " LE, ");
        btl_u(&l, devs.n - le);
        btl_s(&l, " classic), ");
        btl_u(&l, devs.reports);
        btl_s(&l, " reports");
        if (devs.full) {
            btl_s(&l, ", ");
            btl_u(&l, devs.full);
            btl_s(&l, " not kept (list full)");
        }
        say(l.b);
        st = BT_READY;
        go(P_IDLE);
        return;
    }
    default:
        go(P_IDLE);
        return;
    }
}

int bt_step(void)
{
    if (!hal_ok)
        return 1;
    uint64_t t0 = now();
    pump();
    if (st == BT_READY || st == BT_SCANNING)
        btp_step();                         /* the keyboard: its timers, commands and ACL out */
    for (;;) {
        pump();
        if (st != BT_STARTING && st != BT_SCANNING)
            return 1;
        uint64_t t = now();
        if (t < wake) {
            if (wake - t > 2000U || t - t0 > SLICE_US)
                return 0;
            continue;                       /* a short wait: spin */
        }
        wake = 0;
        run();
        if (st != BT_STARTING && st != BT_SCANNING)
            return 1;
        if (now() - t0 > SLICE_US)
            return 0;
    }
}

unsigned bt_wait_ms(void)
{
    uint64_t t = now();
    if (st != BT_STARTING && st != BT_SCANNING)
        return 0;
    unsigned w = issued && c_st == 1 ? 1U : 0U;
    if (wake > t)
        w = (unsigned)((wake - t + 999U) / 1000U);
    unsigned p = st == BT_SCANNING ? btp_poll_ms() : 0U;
    return p && p < w ? p : w;
}

unsigned bt_poll_ms(void)
{
    return hal_ok && (st == BT_READY || st == BT_SCANNING) ? btp_poll_ms() : 0U;
}

int bt_pair(int dev_index)
{
    const struct bt_dev *d = dev_index >= 0 ? bt_dev_at((unsigned)dev_index) : 0;
    if (!hal_ok || !d)
        return -1;
    return btp_pair_dev(d, 1);
}

int bt_state(void)
{
    return st;
}

const char *bt_why(void)
{
    if ((st == BT_READY || st == BT_SCANNING) && bt_pair_state() != BT_PAIR_IDLE)
        return bt_pair_why();
    return why_s;
}

/* ---- the keyboard's glue (bt_pair.h) ---- */
static unsigned h4_lost;
static void p_h4(const uint8_t *p, unsigned n)
{
    if (U.up && bt_uart_write(&U, p, n, 50000))
        return;
    if (!h4_lost++)
        say("bt: keyboard: a packet could not be sent (UART not up, or CTS held for 50 ms)");
}
static uint64_t p_now_ms(void)
{
    return now() / 1000U;
}
static int p_others(void)
{
    return issued && c_st == 1;
}
static int p_ready(void)
{
    return st == BT_READY;
}
static int p_on(void)
{
    return st == BT_READY || st == BT_SCANNING;
}
static uint32_t p_random(void)
{
    uint64_t x = now() * 0x9E3779B97F4A7C15ULL ^ (uint64_t)rx.junk << 17 ^ U.rx_err;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(x >> 29);
}
static const struct btp_io p_io = { p_h4, p_now_ms, say, p_others, p_ready, p_on, p_random };
static void btp_glue_init(void)
{
    btp_init(&p_io);
}

const struct bt_info *bt_info(void)
{
    return &inf;
}

unsigned bt_ndev(void)
{
    if (st != BT_SCANNING && nsorted != devs.n)
        nsorted = bthci_devs_sorted(&devs, sorted, BT_MAX_DEV);
    return st == BT_SCANNING ? devs.n : nsorted;
}

const struct bt_dev *bt_dev_at(unsigned i)
{
    if (st == BT_SCANNING)
        return i < devs.n ? &devs.d[i] : 0;
    return i < nsorted ? &devs.d[sorted[i]] : 0;
}

/* ---- the probe (drvprobe.h) ---- */

static void plog(const char *s)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, "drvprobe: bt: ");
    btl_s(&l, s);
    say(l.b);
}

static int p_kb, p_kb_code, p_kb_ok;        /* level 2's keyboard (below) */
static unsigned p_keys;

static void pass_line(int level)
{
    struct btl l;
    unsigned le = 0;
    for (unsigned i = 0; i < devs.n; i++)
        le += devs.d[i].le;
    btl_init(&l);
    btl_s(&l, "PASS ROM ");
    btl_x(&l, inf.rom.lmp_subver, 4);
    btl_s(&l, " -> patched ");
    btl_x(&l, inf.patched.lmp_subver, 4);
    btl_s(&l, "/");
    btl_x(&l, inf.patched.hci_rev, 4);
    btl_s(&l, " (");
    btl_u(&l, inf.fw_records);
    btl_s(&l, " records), ");
    btl_u(&l, inf.baud);
    btl_s(&l, " baud RTS/CTS, address ");
    btl_s(&l, !inf.addr_read ? "not read" : inf.addr_default ? "Broadcom's default" : "the chip's own");
    btl_s(&l, ", LE ");
    btl_u(&l, le);
    btl_s(&l, " + classic ");
    btl_u(&l, devs.n - le);
    btl_s(&l, " devices");
    if (nsorted && devs.d[sorted[0]].has_rssi) {
        btl_s(&l, ", strongest ");
        btl_i(&l, devs.d[sorted[0]].rssi);
        btl_s(&l, " dBm");
    }
    if (level >= 2 && p_kb_ok) {
        btl_s(&l, "; keyboard paired (");
        btl_s(&l, p_kb_code ? "a code typed" : "Just Works");
        btl_s(&l, "), HID boot protocol, ");
        btl_u(&l, p_keys);
        btl_s(&l, " keys typed, unpaired");
    } else if (level >= 2) {
        btl_s(&l, "; no keyboard tried");
    }
    btl_s(&l, level == 2 ? "; level 2 list above; everything back as found" : "; everything back as found");
    plog(l.b);
}

static void list_devices(unsigned max)
{
    for (unsigned i = 0; i < nsorted && i < max; i++) {
        const struct bt_dev *d = &devs.d[sorted[i]];
        struct btl l;
        btl_init(&l);
        btl_s(&l, "drvprobe: bt:   ");
        btl_s(&l, d->le ? "LE " : "classic ");
        if (d->has_rssi) {
            btl_i(&l, d->rssi);
            btl_s(&l, " dBm");
        } else {
            btl_s(&l, "no RSSI");
        }
        btl_s(&l, ", heard ");
        btl_u(&l, d->hits);
        if (!d->le) {
            btl_s(&l, ", class ");
            btl_x(&l, d->cod, 6);
        }
        btl_s(&l, ", \"");
        btl_s(&l, d->name[0] ? d->name : "(no name)");
        btl_s(&l, "\"");
        say(l.b);
    }
}

#ifdef SBOS_KERNEL
static struct bt_hal k_hal;
#endif

static int p_level = -1, p_scanned;
static uint64_t p_t0;

/* level 2's keyboard: a RAM-only key store (wiped when the probe ends), the typed characters for the log */
#define KB_PAIR_MS 60000U
#define KB_TYPE_MS 20000U
static uint64_t p_kb_t;
static const char *p_kb_why;
static uint8_t pk_addr[6], pk_key[16];
static int pk_have;
static struct btl p_typed;
static unsigned p_lines;

static int pk_same(const uint8_t *a)
{
    for (unsigned i = 0; i < 6; i++)
        if (a[i] != pk_addr[i])
            return 0;
    return 1;
}
static int pk_get(const uint8_t a[6], uint8_t k[16])
{
    if (!pk_have || !pk_same(a))
        return 0;
    bthci_copy(k, pk_key, 16);
    return 1;
}
static void pk_wipe(void)
{
    volatile uint8_t *k = pk_key;
    for (unsigned i = 0; i < 16; i++)
        k[i] = 0;
    pk_have = 0;
}
static void pk_put(const uint8_t a[6], const uint8_t *k)
{
    if (k) {
        bthci_copy(pk_addr, a, 6);
        bthci_copy(pk_key, k, 16);
        pk_have = 1;
    } else if (pk_have && pk_same(a)) {
        pk_wipe();
    }
}
static void p_flush(void)
{
    if (!p_typed.n)
        return;
    struct btl l;
    btl_init(&l);
    btl_s(&l, "drvprobe: bt: typed: ");
    btl_s(&l, p_typed.b);
    say(l.b);
    btl_init(&p_typed);
    p_lines++;
}
static void p_key(int usage, int down, uint32_t uc)
{
    if (!down || (usage >= 0xE0 && usage <= 0xE7))
        return;
    p_keys++;
    if (uc == '\n') {
        btl_s(&p_typed, "<Enter>");
        p_flush();
        return;
    }
    if (p_typed.n > 200)
        p_flush();
    if (uc >= 0x20 && uc < 0x7f) {
        char c[2] = { (char)uc, 0 };
        btl_s(&p_typed, c);
    } else {
        btl_s(&p_typed, uc == 8 ? "<Backspace>" : uc == 9 ? "<Tab>" : uc == 0x1b ? "<Esc>" : "<");
        if (uc != 8 && uc != 9 && uc != 0x1b) {
            btl_x(&p_typed, (uint64_t)usage, 2);
            btl_s(&p_typed, ">");
        }
    }
}

/* level 2 after the scan: pair a keyboard heard in pairing mode, show its code, log 20 s of typing, unpair.
 * 1 while this runs (call again), 0 when done (or nothing to do) */
static int probe_keyboard(unsigned *wait_ms)
{
    uint64_t t = now();
    struct btl l;
    if (p_kb == 0) {
        int idx = -1;
        for (unsigned i = 0; i < bt_ndev(); i++)
            if (bt_is_keyboard(bt_dev_at(i))) {
                idx = (int)i;
                break;
            }
        if (idx < 0) {
            plog("level 2: no keyboard heard in pairing mode (pairing not tried)");
            p_kb = 9;
            return 0;
        }
        bt_set_link_key_store(pk_get, pk_put);
        bt_hid_set_key_cb(p_key);
        btl_init(&p_typed);
        p_keys = p_lines = 0;
        p_kb_code = p_kb_ok = 0;
        p_kb_why = 0;
        btl_init(&l);
        btl_s(&l, "level 2: pairing the keyboard \"");
        btl_s(&l, bt_dev_at((unsigned)idx)->name[0] ? bt_dev_at((unsigned)idx)->name : "(no name)");
        btl_s(&l, "\" (classic, class ");
        btl_x(&l, bt_dev_at((unsigned)idx)->cod, 6);
        btl_s(&l, ")");
        plog(l.b);
        if (bt_pair(idx) < 0) {
            p_kb_why = bt_pair_why();
            p_kb = 3;
        } else {
            p_kb = 1;
            p_kb_t = t;
        }
    }
    (void)bt_step();
    int ps = bt_pair_state();
    if (p_kb == 1) {
        if (ps == BT_PAIR_CODE && !p_kb_code) {
            p_kb_code = 1;
            btl_init(&l);
            btl_s(&l, "type ");
            btl_s(&l, bt_pair_code());
            btl_s(&l, " on the keyboard, then Enter");
            plog(l.b);
        }
        if (ps == BT_PAIR_CONNECTED) {
            plog("level 2: keyboard connected: logging what is typed for 20 s");
            p_kb = 2;
            p_kb_t = t;
        } else if (ps == BT_PAIR_FAILED || ps == BT_PAIR_IDLE) {
            p_kb_why = bt_pair_why();
            p_kb = 3;
        } else if (t - p_kb_t > (uint64_t)KB_PAIR_MS * 1000U) {
            p_kb_why = "The keyboard did not connect within 60 s";
            p_kb = 3;
        }
    } else if (p_kb == 2) {
        if (!bt_hid_connected() || t - p_kb_t > (uint64_t)KB_TYPE_MS * 1000U) {
            p_flush();
            p_kb_ok = 1;
            if (!bt_hid_connected())
                plog("level 2: the keyboard disconnected before the 20 s were over");
            p_kb = 3;
        }
    }
    if (p_kb == 3) {
        bt_unpair();
        p_kb = 4;
        p_kb_t = t;
    }
    if (p_kb == 4) {
        if (bt_pair_state() == BT_PAIR_IDLE || t - p_kb_t > 4000000U) {
            pk_wipe();
            bt_hid_set_key_cb(0);
            bt_set_link_key_store(0, 0);
            p_kb = 9;
            return 0;
        }
    }
    *wait_ms = bt_poll_ms() ? bt_poll_ms() : 10U;
    return 1;
}

int drv_bt_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
#ifdef SBOS_KERNEL
    if (!hal_ok) {
        bt_hal_libsb(&k_hal);
        bt_hal_set(&k_hal);
    }
#endif
    if (!hal_ok)
        return 1;
    if (level == 0) {
        plog("level 0: read only: the BT pins, UART_BT and its clocks, the expander's bus, the 32 kHz buffers, the "
             "firmware file");
        if (bt_look()) {
            struct btl l;
            btl_init(&l);
            btl_s(&l, "PASS read only; firmware ");
            btl_s(&l, inf.fw);
            btl_s(&l, " (");
            btl_u(&l, fwi.records);
            btl_s(&l, " records, \"");
            btl_s(&l, fwi.name[0] ? fwi.name : "no description");
            btl_s(&l, "\")");
            plog(l.b);
        } else {
            struct btl l;
            btl_init(&l);
            btl_s(&l, "FAIL ");
            btl_s(&l, why_s);
            plog(l.b);
        }
        return 1;
    }
    unsigned budget = level >= 2 ? DRV_BT_BUDGET_MS : 45000U;
    if (p_level != level) {
        p_level = level;
        p_scanned = 0;
        p_kb = 0;
        p_t0 = now();
        plog(level >= 2 ? "level 2: on, patched, fast rate; LE scan 12 s + inquiry 10 s, every device listed; then "
                          "off and everything back"
                        : "level 1: on, patched, fast rate; LE scan 5 s + inquiry 5 s; then off and everything back");
        if (st != BT_OFF && st != BT_FAILED) {
            p_level = -1;
            plog("FAIL Bluetooth is in use (on): the probe does not take it over");
            return 1;
        }
        bt_on();
    }
    if (now() - p_t0 > (uint64_t)budget * 1000U) {
        bt_off();
        pk_wipe();
        p_level = -1;
        plog(restored ? "FAIL did not finish within its budget (off, everything back as found)"
                      : "FAIL did not finish within its budget, and NOT everything is back as found");
        return 1;
    }
    if (p_kb && p_kb < 9)
        return probe_keyboard(wait_ms) ? 0 : (*wait_ms = 0, 0);
    if (!bt_step()) {
        *wait_ms = bt_wait_ms();
        return 0;
    }
    if (st == BT_READY && !p_scanned) {
        p_scanned = 1;
        bt_scan(level >= 2 ? 12000U : 5000U, level >= 2 ? 10000U : 5000U);
        return 0;
    }
    if (st == BT_READY && level >= 2 && !p_kb) {
        list_devices(BT_MAX_DEV);
        p_kb = 0;
        if (probe_keyboard(wait_ms))
            return 0;
    }
    if (st == BT_READY) {
        if (level < 2)
            list_devices(3U);
        bt_off();
        p_level = -1;
        if (level >= 2 && p_kb_why && !p_kb_ok) {
            struct btl l;
            btl_init(&l);
            btl_s(&l, "FAIL the scan passed, the keyboard did not: ");
            btl_s(&l, p_kb_why);
            btl_s(&l, restored ? " (unpaired, off, everything back as found)" : " (NOT everything is back as found)");
            plog(l.b);
        } else if (restored) {
            pass_line(level);
        } else {
            plog("FAIL the scan ran, but NOT everything is back as found (see above)");
        }
        return 1;
    }
    {
        struct btl l;
        btl_init(&l);
        btl_s(&l, "FAIL ");
        btl_s(&l, why_s);
        btl_s(&l, restored ? " (off, everything back as found)" : " (and NOT everything is back as found)");
        p_level = -1;
        plog(l.b);
        st = BT_OFF;
    }
    return 1;
}

void drv_bt_abort(void)
{
    if (hal_ok)
        bt_off();
    pk_wipe();
    p_kb = 0;
    p_level = -1;
}

/* ---- the kernel's HAL ---- */
#ifdef SBOS_KERNEL
#include "sb.h"
#include "sb/bcmw_sb.h"
int kfile_read(const char *path, void *dst, uint32_t cap);      /* files.c: the card, read only */

static uint32_t k_rd(uint64_t pa)
{
    uint32_t v = *(volatile uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static int k_probe(uint64_t pa, uint32_t *v)
{
    return sb_probe32(pa, v);
}

static void k_wr(uint64_t pa, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static struct sb_i2c k_bus = { PCAL_BUS, 0, 0, 0, 0 };
static void k_i2c_init(uint32_t fs1, uint32_t fs2, uint32_t fs3)
{
    k_bus.fs1 = fs1;
    k_bus.fs2 = fs2;
    k_bus.fs3 = fs3;
    sb_i2c_bus(&k_bus);
}

static int k_i2c_rd(uint8_t addr, uint8_t reg, uint8_t *v)
{
    return sb_i2c_read(&k_bus, addr, reg, v, 1);
}

static int k_i2c_wr(uint8_t addr, uint8_t reg, uint8_t v)
{
    uint8_t m[2] = { reg, v };
    return sb_i2c_msg(&k_bus, addr, 0, m, 2, 1);
}

static void k_delay(uint32_t us)
{
    sb_udelay(us);
}

static void k_log(const char *s)
{
    sb_log(s);
}

static uint64_t k_now(void)
{
    return sb_us();
}

static long k_card(const char *path, void *dst, uint32_t cap)
{
    return kfile_read(path, dst, cap);
}

static int k_pmic(uint32_t type, uint32_t reg, uint8_t *v)
{
    return sb_pmic_read(type, reg, v);
}

static void *k_alloc(uint32_t n)
{
    return sb_alloc(n);
}

void bt_hal_libsb(struct bt_hal *h)
{
    h->io.rd32 = k_rd;
    h->io.probe32 = k_probe;
    h->io.wr32 = k_wr;
    h->io.i2c_init = k_i2c_init;
    h->io.i2c_rd = k_i2c_rd;
    h->io.i2c_wr = k_i2c_wr;
    h->io.delay_us = k_delay;
    h->io.log = k_log;
    h->now_us = k_now;
    h->vendor_read = bcmw_sb_vendor_read;
    h->card_read = k_card;
    h->pmic_read = k_pmic;
    h->alloc = k_alloc;
}
#endif
