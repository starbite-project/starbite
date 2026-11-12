/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/usb.c (the glue) and usbacm.c (descriptors + control requests), run by usb-test.sh.
 *
 * Fakes: the MAX77705 USB-C block (BC_STATUS, the opcode mailbox and its CONTROL1, libsb's gate around every write),
 * the PHY / PMU / clock-gate and DWC3 status registers (a map; a read of the DWC3 while its clock gate is forced off is
 * a violation: on the phone it could hang the bus), the shared DWC3 driver's API (usbd_*, usb_phy_start) with a fake
 * host on the other end that enumerates through usbacm.c's real usb_ch9_setup, reads IN transfers and types into OUT
 * ones, the kernel timer, the battery, Survival's USB power out, and libsb's log ring (512 lines, as log.c).
 * Checked: the parser, the decision table, the descriptors at both speeds (no mass storage / HID / vendor interface),
 * plug / enumerate / console / unplug, chargers, retries, relinks, Survival, a silent MAX77705, switching off, and the
 * probe at every level: level 0 writes nothing; level 1 leaves every register as found. Exit 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sb.h"
#include "kern.h"
#include "survival.h"
#include "usb.h"
#include "usbacm.h"
#include "usb_dwc3.h"
#include "usb_phy.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- time ---------------------------------------------------------------------------------------------------------- */
static uint64_t now_ms, now_us_extra;
uint64_t sb_ms(void) { return now_ms + now_us_extra / 1000U; }
uint64_t sb_us(void) { return now_ms * 1000U + now_us_extra; }
void sb_udelay(uint64_t us) { now_us_extra += us; }

/* ---- the log ring (log.c's shape) --------------------------------------------------------------------------------- */
#define LOG_LINES 512U
static char ring[LOG_LINES][120];
static unsigned rhead, rcount;
static char all_log[1 << 20];
static size_t all_n;

static unsigned long_lines;
void sb_log(const char *s)
{
    char *l = ring[rhead];
    if (strlen(s) > 110) {      /* log.c keeps 120 columns with the time: longer lines are cut in the ring */
        ++long_lines;
        printf("long log line (%zu): %s\n", strlen(s), s);
    }
    uint64_t ms = sb_ms();
    snprintf(l, 120, "%llu.%02llu %s", (unsigned long long)(ms / 1000U), (unsigned long long)(ms / 10U % 100U), s);
    rhead = (rhead + 1) % LOG_LINES;
    if (rcount < LOG_LINES)
        ++rcount;
    size_t n = strlen(l);
    if (all_n + n + 2 < sizeof(all_log)) {
        memcpy(all_log + all_n, l, n);
        all_n += n;
        all_log[all_n++] = '\n';
        all_log[all_n] = 0;
    }
    if (verbose)
        printf("  log: %s\n", l);
}
void sb_logf(const char *a, const char *b)
{
    char t[512];
    snprintf(t, sizeof(t), "%s%s", a, b);
    sb_log(t);
}
void sb_logx(const char *a, uint64_t v)
{
    char t[512];
    snprintf(t, sizeof(t), "%s0x%0*llx", a, v >> 32 ? 16 : 8, (unsigned long long)v);
    sb_log(t);
}
unsigned sb_log_count(void) { return rcount; }
const char *sb_log_line(unsigned i)
{
    if (i >= rcount)
        return "";
    return ring[(rhead + LOG_LINES - rcount + i) % LOG_LINES];
}
static int log_has(const char *s) { return strstr(all_log, s) != 0; }
static void log_clear(void)
{
    all_n = 0;
    all_log[0] = 0;
}

/* ---- registers --------------------------------------------------------------------------------------------------- */
#define DWC 0x10c00000ULL
#define GATE_TOP 0x1a242064ULL
#define GATE_REF 0x1100206cULL
#define GATE_DPPHY 0x11002008ULL
#define USBCON 0x11100000ULL
static const uint64_t phy_regs[18] = { USBCON + 0x11c, USBCON + 0x110, USBCON + 0x10c, USBCON + 0x108, USBCON + 0x100,
                                       USBCON + 0x08,  USBCON + 0x58,  USBCON + 0x48,  USBCON + 0x04,  USBCON + 0x50,
                                       USBCON + 0x54,  USBCON + 0x5c,  USBCON + 0x20,  0x14060704ULL,  0x1406072cULL,
                                       GATE_DPPHY,     GATE_REF,       GATE_TOP };
#define NREG 64
static struct {
    uint64_t a[NREG];
    uint32_t v[NREG];
    unsigned n;
    uint32_t found[18];
    unsigned writes, phy_writes, dwc_writes, bad_writes, gated_dwc_reads, dwc_reads;
    uint64_t seq, first_restore_seq;
    uint64_t fault_addr;
} G;

static uint32_t *reg(uint64_t a)
{
    for (unsigned i = 0; i < G.n; ++i)
        if (G.a[i] == a)
            return &G.v[i];
    if (G.n >= NREG) {
        printf("register map full\n");
        exit(2);
    }
    G.a[G.n] = a;
    G.v[G.n] = 0;
    return &G.v[G.n++];
}
static int forced_off(uint32_t g) { return (g & (1U << 20)) && !(g & (1U << 21)); }

int sb_probe32(uint64_t addr, uint32_t *v)
{
    if (addr == G.fault_addr)
        return 0;
    if (addr >= DWC && addr < DWC + 0x10000) {
        ++G.dwc_reads;
        if (forced_off(*reg(GATE_TOP)) || forced_off(*reg(GATE_REF)))
            ++G.gated_dwc_reads;
    }
    *v = *reg(addr);
    return 1;
}

void usb_host_wr(uint64_t a, uint32_t v)
{
    ++G.writes;
    ++G.seq;
    int phy = 0;
    for (unsigned i = 0; i < 18; ++i)
        if (phy_regs[i] == a)
            phy = 1;
    if (phy) {
        ++G.phy_writes;
        if (!G.first_restore_seq)
            G.first_restore_seq = G.seq;
    } else if (a >= DWC && a < DWC + 0x10000) {
        ++G.dwc_writes;
        if (a == DWC + 0xc704 && (v & (1U << 30)))
            *reg(DWC + 0xc704) = 0;     /* the soft reset completes at once */
        else
            *reg(a) = v;
        return;
    } else {
        ++G.bad_writes;
    }
    *reg(a) = v;
}

/* S-Boot's state (gates forced off, PHY in SIDDQ), or the loader's (its usb_phy_start values, core soft-reset) */
static void regs_init(int loader_left_it_up)
{
    memset(&G, 0, sizeof(G));
    for (unsigned i = 0; i < 18; ++i) {
        uint32_t v = 0x100U + i;
        if (phy_regs[i] == GATE_TOP || phy_regs[i] == GATE_REF || phy_regs[i] == GATE_DPPHY)
            v = loader_left_it_up ? 0x00300000U : 0x00100000U;
        else if (phy_regs[i] == USBCON + 0x5c)
            v = loader_left_it_up ? 0x00100000U : 0x01100000U;     /* SIDDQ (bit 24); LINESTATE J */
        else if (phy_regs[i] == 0x14060704ULL || phy_regs[i] == 0x1406072cULL)
            v = loader_left_it_up ? 1U : 0U;
        *reg(phy_regs[i]) = v;
        G.found[i] = v;
    }
    *reg(DWC + 0xc120) = 0x5533300aU;   /* GSNPSID */
    *reg(DWC + 0xc110) = 0x2U << 12;    /* GCTL: device */
    *reg(DWC + 0xc704) = 0;             /* DCTL */
    *reg(DWC + 0xc70c) = 1U << 22;      /* DSTS: halted */
}

static int regs_as_found(void)
{
    for (unsigned i = 0; i < 18; ++i) {
        uint32_t m = phy_regs[i] == USBCON + 0x5c ? ~(3U << 20) : 0xffffffffU;
        if ((*reg(phy_regs[i]) ^ G.found[i]) & m) {
            if (verbose)
                printf("  reg %llx is %08x, found %08x\n", (unsigned long long)phy_regs[i], *reg(phy_regs[i]),
                       G.found[i]);
            return 0;
        }
    }
    return 1;
}

/* ---- the MAX77705 USB-C block and libsb's gate ------------------------------------------------------------------- */
static struct {
    uint8_t r[256];
    int silent, no_answer, write_fail;
    uint8_t ctrl1;
    unsigned writes, bad, gate_bad, routes, asks;
    uint8_t wreg[256], wval[256];
    unsigned nw;
} M;
static struct {
    int open;
    uint64_t first, count;
    int tier;
    unsigned steps;
    unsigned opens, closes;
} A;

int sb_usbc_read(uint8_t r, uint8_t *v)
{
    if (M.silent)
        return 0;
    *v = M.r[r];
    return 1;
}

void sb_auth_init(struct sb_auth *a, unsigned space, unsigned lu, uint64_t first, uint64_t count, int tier,
                  unsigned kind, const char *what)
{
    memset(a, 0, sizeof(*a));
    a->magic = 0x5342;
    a->space = space;
    a->lu = lu;
    a->first = first;
    a->count = count;
    a->tier = tier;
    a->kind = kind;
    snprintf(a->what, sizeof(a->what), "%s", what);
    CHECK(strlen(what) < sizeof(a->what), "auth label too long: %s", what);
    CHECK(space == SB_SP_MAX, "auth space %u", space);
}
void sb_auth_step(struct sb_auth *a, unsigned step) { a->steps |= step; }
int sb_gate_open(const struct sb_auth *a)
{
    CHECK(!A.open, "the gate opened twice");
    if ((a->steps & (SB_STEP_WARN | SB_STEP_HELD)) != (SB_STEP_WARN | SB_STEP_HELD))
        return 0;
    A.open = 1;
    A.first = a->first;
    A.count = a->count;
    A.tier = a->tier;
    ++A.opens;
    return 1;
}
void sb_gate_close(void)
{
    A.open = 0;
    ++A.closes;
}

int sb_max_write(unsigned addr7, uint8_t r, uint8_t val, int tier)
{
    uint64_t key = (uint64_t)addr7 << 8 | r;
    if (!A.open || key < A.first || key >= A.first + A.count || tier < 1 || A.tier < 1)
        ++M.gate_bad;
    if (addr7 != 0x25 || (r != 0x21 && r != 0x22 && r != 0x41))
        ++M.bad;
    if (M.silent || M.write_fail)
        return 0;
    ++M.writes;
    if (M.nw < 256) {
        M.wreg[M.nw] = r;
        M.wval[M.nw++] = val;
    }
    M.r[r] = val;
    if (r == 0x41) {
        if (M.r[0x21] == 0x06) {
            M.ctrl1 = M.r[0x22];
            ++M.routes;
        } else if (M.r[0x21] == 0x05) {
            ++M.asks;
            if (!M.no_answer) {
                M.r[0x51] = 0x05;
                M.r[0x52] = M.ctrl1;
            }
        }
    }
    return 1;
}

/* ---- the kernel ---------------------------------------------------------------------------------------------------- */
const int sbos_selftest_enabled = 0;
static struct {
    int armed;
    uint64_t at;
    uint32_t last_ms;
    unsigned sets, cancels;
} T;
int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat)
{
    CHECK(owner == KT_USB && id == 0 && !repeat, "kt_set owner %u id %u repeat %d", owner, id, repeat);
    T.armed = 1;
    T.at = now + ms;
    T.last_ms = ms;
    ++T.sets;
    return 0;
}
void kt_cancel(unsigned owner, unsigned id)
{
    (void)owner;
    (void)id;
    T.armed = 0;
    ++T.cancels;
}
static struct sbos_battery BAT;
void battery_get(struct sbos_battery *b) { *b = BAT; }
static int otg_on;
void surv_usb_status(struct surv_usb *s)
{
    memset(s, 0, sizeof(*s));
    s->state = otg_on ? SURV_USB_ON : SURV_USB_OFF;
}

/* ---- the shared DWC3 driver's API, and a host on the cable ------------------------------------------------------- */
uint8_t usb_host_dma[0x2000];
static struct {
    int phy_on, started, phy_fail, start_stage;
    unsigned phy_starts, starts, stops, cancels;
    uint64_t t_start, stop_seq;
    struct usb_dev_state st;
    struct usbd_ev q[256];
    unsigned qh, qn;
    struct { int on, busy; uintptr_t buf; uint32_t len; } ep[10];
    unsigned in_lens[4096], nin, in_whole_packets;
    /* the host */
    int present, configures, dtr, reads, enumerated, full_speed;
    uint8_t cfg_desc[256], dev_desc[18];
    unsigned cfg_len;
    char got[1 << 18];
    size_t got_n;
} D;

static void push(int type, unsigned ep, uint32_t value, uint32_t status)
{
    if (D.qn >= 256)
        return;
    struct usbd_ev *e = &D.q[(D.qh + D.qn++) % 256];
    e->type = type;
    e->ep = ep;
    e->value = value;
    e->status = status;
}

int usb_phy_start(const char **why)
{
    ++D.phy_starts;
    if (D.phy_fail) {
        *why = "USBCON did not take the PHY settings";
        return -1;
    }
    /* what usb_phy.c does to the registers: gates forced on, PMU released, USBCON programmed */
    for (unsigned i = 0; i < 18; ++i) {
        uint32_t *r = reg(phy_regs[i]);
        if (phy_regs[i] == GATE_TOP || phy_regs[i] == GATE_REF || phy_regs[i] == GATE_DPPHY)
            *r |= 0x00300000U;
        else if (phy_regs[i] == USBCON + 0x5c)
            *r = (*r & ~(1U << 24)) | (2U << 20);   /* SIDDQ off, LINESTATE K */
        else if (phy_regs[i] == 0x14060704ULL || phy_regs[i] == 0x1406072cULL)
            *r |= 1U;
        else
            *r = 0x700U + i;
    }
    D.phy_on = 1;
    *why = 0;
    return 0;
}
void usb_phy_diag(struct tb *t) { tb_s(t, "phy: fake\n"); }

int usbd_start(const char *serial, const char **why)
{
    ++D.starts;
    CHECK(D.phy_on && !forced_off(*reg(GATE_TOP)), "usbd_start without the PHY's clock");
    CHECK(serial && strcmp(serial, "SBOS") == 0, "serial %s", serial ? serial : "(null)");
    if (D.start_stage) {
        *why = D.start_stage == -3 ? "ep0 configuration refused (DEPCMD)" : "DWC3 soft reset stuck (no PHY clock?)";
        if (D.start_stage == -3)
            *reg(DWC + 0xc400) = 0xa7000000U;   /* the event buffer was set before the failure */
        return D.start_stage;
    }
    usb_ch9_init(&D.st, serial);
    memset(D.ep, 0, sizeof(D.ep));
    D.ep[0].on = D.ep[1].on = 1;
    D.started = 1;
    D.t_start = sb_ms();
    D.enumerated = 0;
    D.qh = D.qn = 0;
    *reg(DWC + 0xc704) = 1U << 31;      /* run */
    *reg(DWC + 0xc70c) = 0;
    *reg(DWC + 0xc400) = 0xa7000000U;
    *why = 0;
    return 0;
}

void usbd_stop(void)
{
    if (!D.started)
        return;
    CHECK(!forced_off(*reg(GATE_TOP)), "usbd_stop with the clock gate forced off (the soft reset could not finish)");
    D.started = 0;
    ++D.stops;
    D.stop_seq = ++G.seq;
    memset(D.ep, 0, sizeof(D.ep));
    D.st.config = 0;
    D.qn = 0;
    *reg(DWC + 0xc704) = 0;
    *reg(DWC + 0xc70c) = 1U << 22;
    *reg(DWC + 0xc400) = 0;
}

int usbd_poll(struct usbd_ev *ev)
{
    if (!D.started || !D.qn)
        return 0;
    *ev = D.q[D.qh];
    D.qh = (D.qh + 1) % 256;
    --D.qn;
    return 1;
}

int usbd_queue(unsigned phys, uintptr_t buf, uint32_t len)
{
    if (phys < 2 || phys >= 10 || !D.ep[phys].on || D.ep[phys].busy)
        return -1;
    D.ep[phys].busy = 1;
    D.ep[phys].buf = buf;
    D.ep[phys].len = len;
    if (phys == 5 && D.nin < 4096) {
        D.in_lens[D.nin++] = len;
        if (len && !(len % (D.full_speed ? 64U : 512U)))
            ++D.in_whole_packets;
    }
    return 0;
}
int usbd_busy(unsigned phys) { return phys < 10 && D.ep[phys].busy; }
void usbd_cancel(unsigned phys)
{
    if (phys < 10 && D.ep[phys].busy) {
        D.ep[phys].busy = 0;
        ++D.cancels;
    }
}
int usbd_configured(void) { return D.started && D.st.config == 1; }
int usbd_full_speed(void) { return D.full_speed; }
int usbd_connected(void) { return D.started; }
void usbd_diag(struct tb *t)
{
    CHECK(!forced_off(*reg(GATE_TOP)), "usbd_diag (DWC3 reads) with the clock gate forced off");
    tb_s(t, "dwc3: fake running\ndwc3: events 3\n");
}
void usbd_brief(struct tb *t) { tb_s(t, "RST 1"); }

/* one control transfer through usbacm.c's real request handling, with what usb_dwc3.c does around it */
static struct ch9_res ctl(uint8_t type, uint8_t req, unsigned value, unsigned index, unsigned length,
                          const uint8_t *out)
{
    uint8_t s[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
                     (uint8_t)length, (uint8_t)(length >> 8) };
    struct ch9_res r;
    memset(&r, 0, sizeof(r));
    usb_ch9_setup(&D.st, s, &r);
    if (r.set_config >= 0) {
        const uint8_t *list;
        unsigned n = usb_desc_eps(&list);
        for (unsigned p = 2; p < 10; ++p)
            D.ep[p].on = D.ep[p].busy = 0;
        if (r.set_config == 1)
            for (unsigned i = 0; i < n; ++i)
                D.ep[USB_PHYS(list[i])].on = 1;
        push(USBD_EV_CONFIGURED, 0, (uint32_t)r.set_config, 0);
    }
    if (r.line_changed)
        push(USBD_EV_LINE, 0, D.st.line_state, 0);
    if (r.action == CH9_OUT && out)
        usb_ch9_out(&D.st, s, out, r.len);
    return r;
}

static void host_enumerate(void)
{
    struct ch9_res r;
    push(USBD_EV_RESET, 0, 0, 0);
    push(USBD_EV_CONNECT, 0, D.full_speed ? 1U : 0U, 0);
    D.st.full_speed = D.full_speed;
    r = ctl(0x80, 6, 0x0100, 0, 64, 0);
    memcpy(D.dev_desc, r.data, 18);
    ctl(0x00, 5, 9, 0, 0, 0);
    ctl(0x80, 6, 0x0200, 0, 9, 0);
    r = ctl(0x80, 6, 0x0200, 0, 255, 0);
    D.cfg_len = r.len;
    memcpy(D.cfg_desc, r.data, r.len);
    ctl(0x80, 6, 0x0300, 0, 255, 0);
    ctl(0x80, 6, 0x0302, 0x409, 255, 0);
    ctl(0x80, 6, 0x0303, 0x409, 255, 0);
    ctl(0x80, 6, 0x0f00, 0, 5, 0);      /* BOS: stalled */
    if (D.configures) {
        static const uint8_t lc[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };
        ctl(0x00, 9, 1, 0, 0, 0);
        ctl(0x21, 0x20, 0, 0, 7, lc);
        if (D.dtr)
            ctl(0x21, 0x22, 3, 0, 0, 0);
    }
    D.enumerated = 1;
}

static void host_set_dtr(int on)
{
    D.dtr = on;
    if (D.started && D.st.config == 1)
        ctl(0x21, 0x22, on ? 3U : 0U, 0, 0, 0);
}

static void host_type(const char *s)
{
    size_t n = strlen(s);
    CHECK(D.ep[4].busy, "typing but no OUT transfer is armed");
    if (!D.ep[4].busy)
        return;
    CHECK(D.ep[4].buf == (uintptr_t)usb_host_dma && D.ep[4].len == 512, "OUT transfer buffer / length");
    memcpy(usb_host_dma, s, n);
    D.ep[4].busy = 0;
    push(USBD_EV_DONE, 4, (uint32_t)n, 0);
}

/* 1 ms of the world: the host's side, then the kernel's timer if due */
static void tick(void)
{
    ++now_ms;
    now_us_extra = 0;
    if (D.started && D.present && !D.enumerated && sb_ms() >= D.t_start + 30)
        host_enumerate();
    if (D.started && D.reads && D.ep[5].busy) {
        uint32_t n = D.ep[5].len;
        if (D.got_n + n < sizeof(D.got)) {
            memcpy(D.got + D.got_n, (const void *)D.ep[5].buf, n);
            D.got_n += n;
            D.got[D.got_n] = 0;
        }
        D.ep[5].busy = 0;
        push(USBD_EV_DONE, 5, n, 0);
    }
}
static void run(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        tick();
        if (T.armed && sb_ms() >= T.at) {
            T.armed = 0;
            usb_timer(0);
        }
    }
}
static int got(const char *s) { return strstr(D.got, s) != 0; }
static void got_clear(void)
{
    D.got_n = 0;
    D.got[0] = 0;
}

static void world_reset(int loader_up)
{
    usb_enable(0);
    regs_init(loader_up);
    memset(&M, 0, sizeof(M));
    memset(&A, 0, sizeof(A));
    memset(&D, 0, sizeof(D));
    memset(&T, 0, sizeof(T));
    otg_on = 0;
    D.present = 1;
    D.configures = 1;
    D.dtr = 1;
    D.reads = 1;
    log_clear();
}

/* ---- tests --------------------------------------------------------------------------------------------------------- */
static void test_parse(void)
{
    unsigned a;
    CHECK(usb_cmd_parse("", &a) == USB_CMD_NONE, "empty");
    CHECK(usb_cmd_parse("   ", &a) == USB_CMD_NONE, "spaces");
    CHECK(usb_cmd_parse("help", &a) == USB_CMD_HELP, "help");
    CHECK(usb_cmd_parse("  HELP  ", &a) == USB_CMD_HELP, "HELP");
    CHECK(usb_cmd_parse("?", &a) == USB_CMD_HELP, "?");
    CHECK(usb_cmd_parse("ver", &a) == USB_CMD_VER, "ver");
    CHECK(usb_cmd_parse("bat", &a) == USB_CMD_BAT, "bat");
    CHECK(usb_cmd_parse("battery", &a) == USB_CMD_BAT, "battery");
    CHECK(usb_cmd_parse("usb", &a) == USB_CMD_USB, "usb");
    CHECK(usb_cmd_parse("log", &a) == USB_CMD_LOG && a == 0, "log");
    CHECK(usb_cmd_parse("log 50", &a) == USB_CMD_LOG && a == 50, "log 50");
    CHECK(usb_cmd_parse("log\t7", &a) == USB_CMD_LOG && a == 7, "log tab 7");
    CHECK(usb_cmd_parse("log 0", &a) == USB_CMD_BAD, "log 0");
    CHECK(usb_cmd_parse("log -3", &a) == USB_CMD_BAD, "log -3");
    CHECK(usb_cmd_parse("log 1000000", &a) == USB_CMD_BAD, "log huge");
    CHECK(usb_cmd_parse("log 5 6", &a) == USB_CMD_BAD, "log 5 6");
    CHECK(usb_cmd_parse("follow on", &a) == USB_CMD_FOLLOW && a == 1, "follow on");
    CHECK(usb_cmd_parse("follow OFF", &a) == USB_CMD_FOLLOW && a == 0, "follow off");
    CHECK(usb_cmd_parse("follow", &a) == USB_CMD_BAD, "follow alone");
    CHECK(usb_cmd_parse("follow maybe", &a) == USB_CMD_BAD, "follow maybe");
    CHECK(usb_cmd_parse("ver now", &a) == USB_CMD_BAD, "ver now");
    /* nothing that writes or changes anything exists */
    const char *no[] = { "reboot", "write efs", "dd", "set wifi on", "ufs on", "rm x", "flash", "poke 0x10 1", "off",
                         "menu", "maint", "hash SYSTEM", "logx", "verx" };
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); ++i)
        CHECK(usb_cmd_parse(no[i], &a) == USB_CMD_UNKNOWN, "'%s' should be unknown", no[i]);
    char longline[300];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = 0;
    CHECK(usb_cmd_parse(longline, &a) == USB_CMD_UNKNOWN, "a long word");
}

static void test_decide(void)
{
    CHECK(usb_decide(0, 1, 5000, 0) == USB_OFF, "no VBUS");
    CHECK(usb_decide(1, 1, 0, 0) == USB_UP, "SDP");
    CHECK(usb_decide(1, 2, 0, 0) == USB_UP, "CDP");
    CHECK(usb_decide(1, 3, 9000, 0) == USB_CHARGER, "DCP");
    CHECK(usb_decide(1, 0, 100, 0) == USB_WAIT, "untyped, early");
    CHECK(usb_decide(1, 0, 2000, 0) == USB_UP, "untyped, 2 s");
    CHECK(usb_decide(1, 1, 5000, 1) == USB_OFF, "Survival's power out");
}

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static void check_config(const uint8_t *c, unsigned n, int fs)
{
    CHECK(n == USBACM_CONFIG_LEN && rd16(c + 2) == n, "config length %u, wTotalLength %u", n, rd16(c + 2));
    CHECK(c[4] == 2, "two interfaces, got %u", c[4]);
    unsigned ifs = 0, eps = 0, iads = 0;
    for (unsigned i = 0; i < n;) {
        CHECK(c[i] >= 2 && i + c[i] <= n, "descriptor chain broken at %u", i);
        if (c[i] < 2 || i + c[i] > n)
            break;
        if (c[i + 1] == 4) {
            ++ifs;
            CHECK(c[i + 5] == 0x02 || c[i + 5] == 0x0a, "interface class 0x%02x (only CDC 02 and CDC data 0a)", c[i + 5]);
            CHECK(c[i + 5] != 0x08 && c[i + 5] != 0x03 && c[i + 5] != 0xff, "mass storage / HID / vendor interface");
        }
        if (c[i + 1] == 5) {
            ++eps;
            unsigned mps = rd16(c + i + 4);
            if ((c[i + 3] & 3) == 2)
                CHECK(mps == (fs ? 64U : 512U), "bulk max packet %u at %s speed", mps, fs ? "full" : "high");
            else
                CHECK(c[i + 2] == 0x81 && mps == 16, "interrupt endpoint 0x%02x mps %u", c[i + 2], mps);
        }
        if (c[i + 1] == 11)
            ++iads;
        i += c[i];
    }
    CHECK(ifs == 2 && eps == 3 && iads == 1, "interfaces %u, endpoints %u, IADs %u", ifs, eps, iads);
}

static void test_desc(void)
{
    uint8_t b[256];
    struct usb_dev_state s;
    struct ch9_res r;
    usb_ch9_init(&s, "SBOS");
    CHECK(usb_desc_device(b) == 18 && rd16(b + 8) == 0x1209 && rd16(b + 10) == USBACM_PID && rd16(b + 12) == 0x0300 &&
              rd16(b + 2) == 0x0200 && b[4] == 0xef && b[7] == 64,
          "device descriptor");
    check_config(b, usb_desc_config(b, 0, 2), 0);
    check_config(b, usb_desc_config(b, 1, 2), 1);
    CHECK(usb_desc_qualifier(b) == 10, "qualifier");
    CHECK(usb_desc_string(b, 2, "SBOS") && b[2] == 'S' && b[1] == 3, "product string");
    CHECK(usb_desc_string(b, 3, "SBOS") == 2 + 2 * 4 && b[2] == 'S' && b[8] == 'S', "serial string");
    CHECK(usb_desc_string(b, 5, "SBOS") == 0 && usb_desc_string(b, 0xee, "SBOS") == 0, "no other strings");
    const uint8_t *list;
    CHECK(usb_desc_eps(&list) == 3 && list[0] == 0x81 && list[1] == 0x82 && list[2] == 0x02, "endpoint list");
    for (unsigned i = 0; i < 3; ++i)
        CHECK(USB_PHYS(list[i]) < 10, "endpoint 0x%02x beyond usb_dwc3.c's 10 physical endpoints", list[i]);

    usbacm_seen_reset();
#define SETUP(t, q, v, i, l)                                                                                           \
    do {                                                                                                               \
        uint8_t st_[8] = { t, q, (uint8_t)(v), (uint8_t)((v) >> 8), (uint8_t)(i), (uint8_t)((i) >> 8), (uint8_t)(l),     \
                           (uint8_t)((l) >> 8) };                                                                      \
        usb_ch9_setup(&s, st_, &r);                                                                                    \
    } while (0)
    SETUP(0x80, 0, 0, 0, 2);
    CHECK(r.action == CH9_IN && r.len == 2 && r.data[0] == 1, "GET_STATUS device: self-powered");
    SETUP(0x80, 6, 0x0100, 0, 8);
    CHECK(r.action == CH9_IN && r.len == 8, "device descriptor cut to wLength 8");
    SETUP(0x00, 5, 12, 0, 0);
    CHECK(r.action == CH9_ACK && r.set_address == 12 && usbacm_seen.address == 12, "SET_ADDRESS");
    SETUP(0x00, 5, 200, 0, 0);
    CHECK(r.action == CH9_STALL, "SET_ADDRESS 200 stalls");
    SETUP(0x00, 9, 2, 0, 0);
    CHECK(r.action == CH9_STALL && r.set_config < 0, "SET_CONFIGURATION 2 stalls");
    SETUP(0x00, 9, 1, 0, 0);
    CHECK(r.action == CH9_ACK && r.set_config == 1 && s.config == 1, "SET_CONFIGURATION 1");
    SETUP(0x80, 8, 0, 0, 1);
    CHECK(r.action == CH9_IN && r.data[0] == 1, "GET_CONFIGURATION");
    SETUP(0x81, 10, 0, 1, 1);
    CHECK(r.action == CH9_IN, "GET_INTERFACE 1");
    SETUP(0x81, 10, 0, 2, 1);
    CHECK(r.action == CH9_STALL, "GET_INTERFACE 2 (none) stalls");
    SETUP(0x01, 11, 0, 2, 0);
    CHECK(r.action == CH9_STALL, "SET_INTERFACE 2 stalls");
    SETUP(0xa1, 0xfe, 0, 2, 1);
    CHECK(r.action == CH9_STALL, "mass storage GET_MAX_LUN stalls");
    SETUP(0x21, 0xff, 0, 2, 0);
    CHECK(r.action == CH9_STALL, "bulk-only reset stalls");
    SETUP(0x81, 6, 0x2200, 3, 64);
    CHECK(r.action == CH9_STALL, "HID report descriptor stalls");
    SETUP(0xc0, 0x01, 0, 0, 16);
    CHECK(r.action == CH9_STALL, "vendor request stalls");
    SETUP(0x80, 6, 0x0f00, 0, 5);
    CHECK(r.action == CH9_STALL, "BOS stalls");
    SETUP(0x80, 6, 0x03ee, 0, 18);
    CHECK(r.action == CH9_STALL, "Microsoft OS string stalls");
    SETUP(0x21, 0x20, 0, 0, 7);
    CHECK(r.action == CH9_OUT && r.len == 7, "SET_LINE_CODING");
    {
        uint8_t st[8] = { 0x21, 0x20, 0, 0, 0, 0, 7, 0 }, lc[7] = { 0x80, 0x25, 0, 0, 0, 0, 8 };
        usb_ch9_out(&s, st, lc, 7);
    }
    SETUP(0xa1, 0x21, 0, 0, 7);
    CHECK(r.action == CH9_IN && r.len == 7 && r.data[0] == 0x80 && r.data[1] == 0x25, "GET_LINE_CODING 9600");
    SETUP(0x21, 0x22, 3, 0, 0);
    CHECK(r.action == CH9_ACK && r.line_changed && s.line_state == 3 && usbacm_seen.line_state == 3, "DTR + RTS");
    SETUP(0x21, 0x22, 1, 1, 0);
    CHECK(r.action == CH9_STALL, "line state on the data interface stalls");
    SETUP(0x02, 1, 0, 0x82, 0);
    CHECK(r.action == CH9_ACK && r.clear_halt == 0x82, "CLEAR_FEATURE halt 0x82");
    SETUP(0x00, 9, 1, 0, 4);
    CHECK(r.action == CH9_STALL, "a no-data request with wLength stalls");
#undef SETUP
}

static void test_lifecycle(void)
{
    struct usb_status st;
    world_reset(0);
    BAT.ok = 1;
    BAT.pct_x10 = 853;
    BAT.mv = 3950;
    BAT.ma = 450;
    BAT.avg_ma = 430;
    BAT.temp_c10 = 312;
    BAT.charger = 1;

    usb_enable(1);
    CHECK(T.armed, "enable arms the timer");
    run(5000);
    usb_status(&st);
    CHECK(st.state == USB_OFF && D.phy_starts == 0 && M.writes == 0 && G.writes == 0, "no cable: nothing touched");
    CHECK(T.last_ms >= 900 && T.last_ms <= 1000, "no cable: the cable is read once a second (%u ms)", T.last_ms);
    CHECK(!strcmp(usb_why(), "Not connected"), "why: %s", usb_why());

    /* a computer */
    M.r[0x08] = 0x81;
    run(1100);
    usb_status(&st);
    CHECK(D.phy_starts == 1 && D.starts == 1 && D.started, "plugged into a computer: PHY and core up");
    CHECK(M.routes >= 1 && M.ctrl1 == 0x09, "D+/D- switch written COM_USB");
    CHECK(M.nw >= 3 && M.wreg[0] == 0x21 && M.wval[0] == 0x06 && M.wreg[1] == 0x22 && M.wval[1] == 0x09 &&
              M.wreg[2] == 0x41 && M.wval[2] == 0,
          "the route write is opcode 0x06, COM_USB, the end marker");
    CHECK(M.asks >= 1 && st.ctrl1 == 0x09 && log_has("reads back"), "CONTROL1 read back 0x09");
    CHECK(M.bad == 0 && M.gate_bad == 0 && !A.open && A.opens == A.closes, "every MAX77705 write gated, in the mailbox");
    CHECK(st.configured && st.address == 9 && st.terminal, "enumerated: configured, address 9, terminal open");
    CHECK(D.cfg_len == USBACM_CONFIG_LEN, "the host read a %u-byte configuration", D.cfg_len);
    check_config(D.cfg_desc, D.cfg_len, 0);
    CHECK(got("Starbite OS " SBOS_VERSION " console (read only)"), "banner");
    CHECK(G.gated_dwc_reads == 0, "no DWC3 read with its clock gated");

    /* the console */
    got_clear();
    host_type("help\r");
    run(20);
    CHECK(got("commands (this console only reads"), "help");
    got_clear();
    host_type("ver\r\n");
    run(20);
    CHECK(got("Starbite OS " SBOS_VERSION ", up "), "ver: [%s]", D.got);
    host_type("\n");
    run(20);
    got_clear();
    host_type("bat\r");
    run(20);
    CHECK(got("battery 85.3 %, 3950 mV, 450 mA (average 430 mA), 31.2 C, charger USB"), "bat: [%s]", D.got);
    got_clear();
    host_type("vex\x7f\x7f");
    run(5);
    host_type("er\r");
    run(20);
    CHECK(got("\b \b") && got("Starbite OS " SBOS_VERSION ", up "), "backspace editing");
    got_clear();
    host_type("reboot\r");
    run(20);
    CHECK(got("unknown command"), "reboot is unknown");
    got_clear();
    host_type("usb\r");
    run(20);
    CHECK(got("Connected to a computer") || got("console open"), "usb: [%s]", D.got);
    CHECK(got("D+/D- switch 0x09"), "usb shows the switch");
    got_clear();
    sb_log("hello from the test");
    run(30);
    CHECK(got("hello from the test\r\n"), "follow streams a new log line");
    got_clear();
    host_type("follow off\r");
    run(20);
    sb_log("quiet line");
    run(30);
    CHECK(got("follow off") && !got("quiet line"), "follow off");
    got_clear();
    host_type("log 1\r");
    run(40);
    CHECK(got("quiet line") && !got("hello from the test"), "log 1 replays the last line");
    got_clear();
    host_type("log 2\r");
    run(40);
    CHECK(got("quiet line") && got("hello from the test"), "log 2 replays the last two lines");
    got_clear();
    host_type("log\r");
    run(400);
    CHECK(got("usb: watching the cable") && got("quiet line"), "log replays the ring from its start");
    host_type("follow on\r");
    run(20);

    /* a flood the host does not read: the TX ring fills, the log ring wraps, a "lost" line says so */
    D.reads = 0;
    for (unsigned i = 0; i < 1500; ++i) {
        char t[64];
        snprintf(t, sizeof(t), "flood line %u with some words to fill it up", i);
        sb_log(t);
        run(1);
    }
    got_clear();
    D.reads = 1;
    run(2000);
    CHECK(got("flood line 1499"), "the flood's last line got out");
    CHECK(got("[log lines were lost"), "the wrap was said");
    CHECK(D.in_whole_packets == 0, "no IN transfer of whole packets only (%u)", D.in_whole_packets);
    for (unsigned i = 0; i < D.nin; ++i)
        CHECK(D.in_lens[i] >= 1 && D.in_lens[i] <= 4096, "IN length %u", D.in_lens[i]);

    /* the terminal closes: the IN in flight cancelled, nothing more sent */
    D.reads = 0;
    sb_log("one more");
    run(10);
    host_set_dtr(0);
    run(10);
    usb_status(&st);
    CHECK(!st.terminal, "DTR off closes the console");
    CHECK(!D.ep[5].busy, "no IN left in flight after DTR off");
    D.reads = 1;
    got_clear();
    sb_log("after close");
    run(50);
    CHECK(!got("after close"), "nothing sent with the port closed");
    host_set_dtr(1);
    run(50);
    CHECK(got("console (read only)"), "DTR on again: a new banner");

    /* unplug: two low readings, then everything as found */
    unsigned stops = D.stops;
    M.r[0x08] = 0x00;
    run(1200);
    usb_status(&st);
    CHECK(D.stops == stops + 1 && !D.started, "unplug: usbd_stop");
    CHECK(regs_as_found(), "unplug: PHY / PMU / gates as found");
    CHECK(G.first_restore_seq > D.stop_seq || G.phy_writes == 0, "the PHY went back after the core stopped");
    CHECK(G.bad_writes == 0 && G.dwc_writes == 0, "no write outside the PHY's registers (%u, %u)", G.bad_writes,
          G.dwc_writes);
    CHECK(st.state == USB_OFF && !st.configured && !st.terminal, "unplug: off");
    CHECK(G.gated_dwc_reads == 0, "no DWC3 read with its clock gated");

    /* switched off while up */
    M.r[0x08] = 0x81;
    run(1500);
    CHECK(D.started, "replug: up again");
    CHECK(D.phy_starts == 2, "replug: the PHY from the start");
    usb_enable(0);
    CHECK(!D.started && regs_as_found() && !T.armed, "usb_enable(0): stopped, as found, no timer");
}

static void test_chargers(void)
{
    struct usb_status st;
    world_reset(0);
    usb_enable(1);
    M.r[0x08] = 0x83;   /* DCP */
    run(5000);
    usb_status(&st);
    CHECK(st.state == USB_CHARGER && D.starts == 0 && M.writes == 0, "a wall charger: no link, no write");
    CHECK(!strcmp(usb_why(), "Charger (no data)"), "why: %s", usb_why());

    world_reset(0);
    usb_enable(1);
    M.r[0x08] = 0x80;   /* VBUS, type not known */
    run(1500);
    CHECK(D.starts == 0, "untyped: waits");
    run(1500);
    CHECK(D.starts == 1, "untyped: starts after 2 s");

    world_reset(0);
    otg_on = 1;
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(3000);
    CHECK(D.starts == 0, "Survival's power out is not a computer");
    otg_on = 0;
    run(1500);
    CHECK(D.started, "after Survival: a computer");
    otg_on = 1;
    run(700);
    CHECK(!D.started && regs_as_found(), "Survival's power out on: torn down at once");
    otg_on = 0;
}

static void test_failures(void)
{
    struct usb_status st;
    /* the PHY does not start: down, as found, again after 5 then 10 s */
    world_reset(0);
    D.phy_fail = 1;
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(1100);
    usb_status(&st);
    CHECK(st.state == USB_RETRY && D.phy_starts == 1 && regs_as_found(), "PHY failure: retry state, as found");
    CHECK(log_has("did not start: USBCON did not take the PHY settings"), "PHY failure logged");
    run(5000);
    CHECK(D.phy_starts == 2, "retried after 5 s (%u)", D.phy_starts);
    run(8000);
    CHECK(D.phy_starts == 2, "not before 10 s more");
    run(2500);
    CHECK(D.phy_starts == 3, "retried after 10 s more (%u)", D.phy_starts);
    D.phy_fail = 0;
    run(20500);
    CHECK(D.started, "a later retry starts");

    /* usbd_start stage -3: the event buffer scrubbed by hand */
    world_reset(1);
    D.start_stage = -3;
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(1100);
    CHECK(*reg(DWC + 0xc400) == 0 && G.dwc_writes >= 4, "stage -3: the event buffer cleared, the core soft reset");
    CHECK(regs_as_found(), "stage -3: as found");

    /* a host that never configures (a Mac holding a new accessory): restarts at 10, then 20 s */
    world_reset(0);
    D.configures = 0;
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(12000);
    usb_status(&st);
    CHECK(st.relinks == 1, "restarted once after 10 s (%u)", st.relinks);
    run(20500);
    usb_status(&st);
    CHECK(st.relinks == 2, "again after 20 s (%u)", st.relinks);
    CHECK(M.routes >= 4, "the switch re-written (%u)", M.routes);
    CHECK(!strcmp(usb_why(), "Waiting for the computer"), "why: %s", usb_why());

    /* a MAX77705 that stops answering: nothing starts; one that never answers the read-back: the link anyway */
    world_reset(0);
    M.silent = 1;
    usb_enable(1);
    run(4000);
    CHECK(D.starts == 0, "silent MAX77705: nothing");
    world_reset(0);
    M.no_answer = 1;
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(1500);
    usb_status(&st);
    CHECK(D.started && st.ctrl1 < 0 && log_has("did not answer the CONTROL1 read"), "no read-back: up anyway");
    world_reset(0);
    M.write_fail = 1;
    M.r[0x08] = 0x81;
    usb_enable(1);
    run(1500);
    CHECK(D.started && log_has("could not be set"), "switch write failed: up anyway, said");
    world_reset(0);
    usb_enable(1);
    M.r[0x08] = 0x81;
    run(1500);
    CHECK(D.started, "up");
    M.silent = 1;
    run(2500);
    CHECK(!D.started && regs_as_found(), "the MAX77705 went quiet while up: down, as found");
    usb_enable(0);
}

static int probe_run(int level, unsigned limit_ms, unsigned *calls)
{
    unsigned w = 0;
    uint64_t t0 = sb_ms();
    *calls = 0;
    for (;;) {
        ++*calls;
        if (drv_usb_probe(level, &w))
            return (int)(sb_ms() - t0);
        if (sb_ms() - t0 > limit_ms)
            return -1;
        for (unsigned i = 0; i < (w ? w : 1U); ++i)
            tick();
    }
}

static void test_probe(void)
{
    unsigned calls;
    int ms;
    /* level 0, S-Boot's state: the DWC3 not read (clock gated), nothing written */
    world_reset(0);
    M.r[0x08] = 0x81;
    ms = probe_run(0, 1000, &calls);
    CHECK(ms >= 0 && calls == 1, "level 0: one call");
    CHECK(G.writes == 0 && M.nw == 0 && D.phy_starts == 0 && D.starts == 0, "level 0 writes nothing");
    CHECK(G.dwc_reads == 0 && G.gated_dwc_reads == 0, "level 0: no DWC3 read with the gate forced off");
    CHECK(log_has("drvprobe: usb: PASS DWC3 clock gated (not read), PHY powered down, VBUS in from a computer (SDP)"),
          "level 0 PASS line (gated)");

    /* level 0, as the loader leaves it */
    world_reset(1);
    ms = probe_run(0, 1000, &calls);
    CHECK(G.writes == 0 && M.nw == 0, "level 0 writes nothing (loader state)");
    CHECK(G.dwc_reads >= 7, "level 0 reads the DWC3 when clocked");
    CHECK(log_has("drvprobe: usb: PASS DWC3 id 0x5533300A stopped, PHY powered, no VBUS"), "level 0 PASS line: %s",
          strstr(all_log, "drvprobe: usb: PASS") ? strstr(all_log, "drvprobe: usb: PASS") : "none");
    CHECK(log_has("run/stop off, halted, no event buffer"), "level 0 explains the loader's state");

    world_reset(1);
    M.silent = 1;
    probe_run(0, 1000, &calls);
    CHECK(log_has("drvprobe: usb: FAIL the MAX77705 USB-C block did not answer"), "level 0 FAIL: MAX77705");
    world_reset(1);
    *reg(DWC + 0xc120) = 0xdeadbeef;
    probe_run(0, 1000, &calls);
    CHECK(log_has("drvprobe: usb: FAIL no DWC3 at 0x10c00000"), "level 0 FAIL: GSNPSID");
    world_reset(1);
    G.fault_addr = DWC + 0xc70c;
    probe_run(0, 1000, &calls);
    CHECK(log_has("drvprobe: usb: FAIL a DWC3 register read faulted"), "level 0 FAIL: fault");

    /* level 1: a Mac that enumerates */
    world_reset(0);
    M.r[0x08] = 0x81;
    D.dtr = 0;
    ms = probe_run(1, DRV_USB_BUDGET_MS, &calls);
    CHECK(ms >= 0 && ms < 14000, "level 1 finished in %d ms", ms);
    CHECK(log_has("drvprobe: usb: PASS enumerated: address 9, configured, high speed"), "level 1 PASS line");
    CHECK(D.stops == 1 && !D.started && regs_as_found(), "level 1: stopped, as found");
    CHECK(M.bad == 0 && M.gate_bad == 0 && G.bad_writes == 0, "level 1: only the allowed writes");
    CHECK(G.gated_dwc_reads == 0, "level 1: no DWC3 read with its clock gated");
    CHECK(log_has("drvprobe: usb: dwc3: fake running"), "level 1 dumps the driver's diagnostics");

    /* level 1, loader state: nothing to write back */
    world_reset(1);
    M.r[0x08] = 0x82;   /* CDP */
    probe_run(1, DRV_USB_BUDGET_MS, &calls);
    CHECK(log_has("drvprobe: usb: PASS enumerated"), "level 1 PASS from the loader's state");
    CHECK(regs_as_found(), "level 1 (loader state): as found");

    /* level 1: no cable / a charger: skipped, nothing touched */
    world_reset(0);
    probe_run(1, 1000, &calls);
    CHECK(log_has("drvprobe: usb: PASS level 1 skipped: no USB cable") && G.writes == 0 && M.nw == 0,
          "level 1 without VBUS: skipped");
    world_reset(0);
    M.r[0x08] = 0x83;
    probe_run(1, 1000, &calls);
    CHECK(log_has("drvprobe: usb: PASS level 1 skipped: a wall charger") && G.writes == 0, "level 1 on a charger");

    /* level 1: a host that never configures */
    world_reset(0);
    M.r[0x08] = 0x81;
    D.configures = 0;
    ms = probe_run(1, DRV_USB_BUDGET_MS, &calls);
    CHECK(ms >= 10000 && ms < 14000, "never configured: ends at 10 s (%d)", ms);
    CHECK(log_has("drvprobe: usb: FAIL the host read") && log_has("never configured it"), "never configured: FAIL");
    CHECK(regs_as_found() && !D.started, "never configured: as found");

    /* level 1: no host at all */
    world_reset(0);
    M.r[0x08] = 0x81;
    D.present = 0;
    probe_run(1, DRV_USB_BUDGET_MS, &calls);
    CHECK(log_has("drvprobe: usb: FAIL up for 10 s but the host sent nothing"), "no host: FAIL");
    CHECK(regs_as_found(), "no host: as found");

    /* level 1: the PHY fails */
    world_reset(0);
    M.r[0x08] = 0x81;
    D.phy_fail = 1;
    probe_run(1, DRV_USB_BUDGET_MS, &calls);
    CHECK(log_has("drvprobe: usb: FAIL USBCON did not take the PHY settings"), "PHY failure: FAIL with the why");
    CHECK(regs_as_found() && G.gated_dwc_reads == 0, "PHY failure: as found, no gated read");

    /* level 1: the cable pulled */
    world_reset(0);
    M.r[0x08] = 0x81;
    D.configures = 0;
    {
        unsigned w = 0;
        drv_usb_probe(1, &w);
        for (unsigned i = 0; i < 300; ++i) {
            tick();
            drv_usb_probe(1, &w);
        }
        M.r[0x08] = 0;
        int done = 0;
        for (unsigned i = 0; i < 3000 && !done; ++i) {
            tick();
            done = drv_usb_probe(1, &w);
        }
        CHECK(done && log_has("FAIL the cable was pulled"), "cable pulled: FAIL");
        CHECK(regs_as_found(), "cable pulled: as found");
    }

    /* abort at any point */
    world_reset(0);
    M.r[0x08] = 0x81;
    {
        unsigned w = 0;
        drv_usb_probe(1, &w);
        for (unsigned i = 0; i < 1000; ++i) {
            tick();
            drv_usb_probe(1, &w);
        }
        drv_usb_abort();
        CHECK(!D.started && regs_as_found(), "abort: stopped, as found");
        drv_usb_abort();
        CHECK(regs_as_found(), "abort twice: fine");
        ms = probe_run(1, DRV_USB_BUDGET_MS, &calls);
        CHECK(ms >= 0 && regs_as_found(), "after an abort a new level 1 runs from the start");
    }
    world_reset(0);
    drv_usb_abort();
    CHECK(G.writes == 0 && M.nw == 0, "abort with nothing running touches nothing");

    /* level 2 */
    world_reset(0);
    ms = probe_run(2, 100, &calls);
    CHECK(calls == 1 && log_has("drvprobe: usb: PASS level 2"), "level 2: nothing");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    usb_init();             /* SBOS_USB_AUTO 0: logs and stays off */
    CHECK(!T.armed && log_has("usb: off"), "usb_init with SBOS_USB_AUTO 0 stays off");
    test_parse();
    test_decide();
    test_desc();
    test_lifecycle();
    test_chargers();
    test_failures();
    test_probe();
    CHECK(long_lines == 0, "%u log lines longer than the ring keeps", long_lines);
    printf("usb-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
