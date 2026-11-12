/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/bt.c, bt_uart.c, bt_hci.c and pcal6524.c (bt-test.sh): a fake register file, a fake UART_BT
 * (FIFO, loopback timed by its divisor, a baud check against the chip's rate), a fake PCAL6524 behind a fake I2C,
 * and a scripted fake BCM4361 Bluetooth controller answering HCI over H4 (reset, version, the patch commands with
 * every record checked in order, the baud change, Read_BD_ADDR, LE advertising reports with AD names, inquiry
 * results with EIR). Every probe level, the faults, and "everything back as found" after each run. -v prints the log.
 * The .hcd is synthetic; no vendor file is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt.h"
#include "bt_uart.h"

static int verbose, failures, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

/* ---- time ---- */
static uint64_t T;
static uint64_t f_now(void) { return T++; }
static void f_delay(uint32_t us) { T += us; }

/* ---- the log ---- */
static char logbuf[1 << 20];
static size_t loglen;
static void f_log(const char *s)
{
    size_t n = strlen(s);
    if (loglen + n + 2 < sizeof(logbuf)) {
        memcpy(logbuf + loglen, s, n);
        loglen += n;
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
    if (verbose)
        printf("  | %s\n", s);
}
static int logged(const char *s) { return strstr(logbuf, s) != 0; }
static void log_clear(void) { loglen = 0; logbuf[0] = 0; }

/* ---- registers ---- */
#define NREG 512
static uint64_t ra[NREG];
static uint32_t rv[NREG];
static unsigned nreg;
static uint64_t bad_write;                  /* the first write outside the test's own allow-list */
static int writes;

static uint32_t *slot(uint64_t a)
{
    for (unsigned i = 0; i < nreg; i++)
        if (ra[i] == a)
            return &rv[i];
    if (nreg == NREG)
        abort();
    ra[nreg] = a;
    rv[nreg] = 0;
    return &rv[nreg++];
}
static uint32_t reg(uint64_t a) { return *slot(a); }
static void set(uint64_t a, uint32_t v) { *slot(a) = v; }

#define GPQ0 0x140500A0ULL
#define GPA2 0x14050060ULL
#define GPG2 0x104300C0ULL
#define GPG0 0x10430080ULL
#define GPG3 0x108300C0ULL
#define GPP6 0x10830040ULL
#define USI10 0x1081103CULL
#define CMU1 0x10800000ULL

/* ---- the fake PCAL6524 ---- */
static uint8_t x_out[3], x_cfg[3], x_other_in[3];
static int x_bus_up, x_reset_prev = -1, x_other_changed;
static uint8_t x_out0[3], x_cfg0[3];
static int x_in_reset(void)
{
    return ((reg(GPG3) >> 24) & 0xf) != 1 || !((reg(GPG3 + 4) >> 6) & 1);
}
static void x_check_reset(void)
{
    int r = x_in_reset();
    if (r && x_reset_prev == 0) {           /* into reset: the chip's defaults */
        memset(x_out, 0, 3);
        memset(x_cfg, 0xff, 3);
    }
    x_reset_prev = r;
}
static int f_i2c_rd(uint8_t addr, uint8_t r, uint8_t *v)
{
    x_check_reset();
    T += 100;
    if (!x_bus_up || addr != 0x22 || x_in_reset())
        return 0;
    if (r <= 2)
        *v = (uint8_t)((x_out[r] & ~x_cfg[r]) | (x_other_in[r] & x_cfg[r]));
    else if (r >= 4 && r <= 6)
        *v = x_out[r - 4];
    else if (r >= 0x0c && r <= 0x0e)
        *v = x_cfg[r - 0x0c];
    else
        *v = 0;
    return 1;
}
static int f_i2c_wr(uint8_t addr, uint8_t r, uint8_t v)
{
    x_check_reset();
    T += 100;
    if (!x_bus_up || addr != 0x22 || x_in_reset())
        return 0;
    uint8_t *p = r >= 4 && r <= 6 ? &x_out[r - 4] : r >= 0x0c && r <= 0x0e ? &x_cfg[r - 0x0c] : 0;
    if (!p)
        return 0;
    uint8_t mask = (r == 4 || r == 0x0c) ? 0x80 : 0x00;
    if ((*p ^ v) & ~mask)
        x_other_changed = 1;
    *p = v;
    return 1;
}
static void f_i2c_init(uint32_t a, uint32_t b, uint32_t c)
{
    (void)a, (void)b, (void)c;
    x_bus_up = 1;
}
static int chip_powered(void)
{
    x_check_reset();
    return !x_in_reset() && (x_out[0] & 0x80) && !(x_cfg[0] & 0x80);
}

/* ---- the fake controller ---- */
enum { M_NORMAL = 0, M_SILENT, M_BAUDFAIL, M_LAUNCH_SILENT, M_REFUSE_RECORD };
static int c_mode, c_was_powered, c_patched, c_download, c_rec_idx, c_rec_bad, c_downloads, c_resets;
static uint32_t c_baud = 115200;
static uint8_t c_cmd[300];
static unsigned c_have;
static const uint8_t *hcd;
static uint32_t hcd_len;
static int c_default_addr;
static unsigned c_powerups;

struct sched { uint64_t t; uint8_t b[260]; unsigned n; };
static struct sched sch[64];
static unsigned nsch;

/* the UART's receive side */
static struct { uint64_t t; uint8_t b; } rxq[8192];
static unsigned rx_head, rx_tail;
static uint64_t tx_done;
static int uart_clock_on = 1;
static uint32_t uart_clk = 200000000;

static uint32_t host_baud(void)
{
    uint32_t div = 16 * (reg(BTU_BASE + BTU_UBRDIV) + 1) + reg(BTU_BASE + BTU_UFRACVAL);
    return uart_clk / div;
}
static int rates_match(void)
{
    uint32_t h = host_baud();
    uint32_t e = h > c_baud ? h - c_baud : c_baud - h;
    return (uint64_t)e * 100 <= (uint64_t)c_baud * 3;
}
static void rx_push(uint64_t t, uint8_t b)
{
    if (rx_tail - rx_head < 8192) {
        rxq[rx_tail % 8192].t = t;
        rxq[rx_tail % 8192].b = b;
        rx_tail++;
    }
}
/* bytes from the chip: garbled when the two rates differ */
static void c_send(const uint8_t *p, unsigned n, uint64_t at)
{
    for (unsigned i = 0; i < n; i++)
        rx_push(at, rates_match() ? p[i] : (uint8_t)(0xA5 ^ p[i]));
}
static void c_event(const uint8_t *ev, unsigned n, uint64_t at)
{
    uint8_t b[300];
    b[0] = 4;
    memcpy(b + 1, ev, n);
    c_send(b, n + 1, at);
}
static void c_cc(uint16_t op, const uint8_t *ret, unsigned rn)
{
    uint8_t ev[300] = { 0x0E, (uint8_t)(3 + 1 + rn), 1, (uint8_t)op, (uint8_t)(op >> 8), 0 };
    memcpy(ev + 6, ret, rn);
    c_event(ev, 6 + rn, T + 50);
}
static void c_cs(uint16_t op, uint8_t status)
{
    uint8_t ev[6] = { 0x0F, 4, status, 1, (uint8_t)op, (uint8_t)(op >> 8) };
    c_event(ev, 6, T + 50);
}
static void schedule(uint64_t at, const uint8_t *ev, unsigned n)
{
    if (nsch < 64) {
        sch[nsch].t = at;
        sch[nsch].b[0] = 4;
        memcpy(sch[nsch].b + 1, ev, n);
        sch[nsch].n = n + 1;
        nsch++;
    }
}
static void le_report(uint64_t at, uint8_t a0, int8_t rssi, const char *name, int shortened)
{
    uint8_t ev[64] = { 0x3E, 0, 0x02, 1, 0x00, 0x01, a0, 0x11, 0x22, 0x33, 0x44, 0xC5 };
    unsigned n = 12, dl = 0;
    uint8_t ad[40];
    ad[dl++] = 2, ad[dl++] = 0x01, ad[dl++] = 0x06;           /* flags */
    if (name) {
        ad[dl++] = (uint8_t)(1 + strlen(name));
        ad[dl++] = shortened ? 0x08 : 0x09;
        memcpy(ad + dl, name, strlen(name));
        dl += (unsigned)strlen(name);
    }
    ev[n++] = (uint8_t)dl;
    memcpy(ev + n, ad, dl);
    n += dl;
    ev[n++] = (uint8_t)rssi;
    ev[1] = (uint8_t)(n - 2);
    schedule(at, ev, n);
}
static void inquiry_eir(uint64_t at, uint8_t a0, int8_t rssi, const char *name)
{
    uint8_t ev[2 + 255] = { 0x2F, 255, 1, a0, 0x10, 0x20, 0x30, 0x40, 0x50, 1, 0, 0x14, 0x04, 0x24, 0x00, 0x00 };
    ev[2 + 1 + 13] = (uint8_t)rssi;
    uint8_t *eir = ev + 2 + 15;
    eir[0] = (uint8_t)(1 + strlen(name));
    eir[1] = 0x09;
    memcpy(eir + 2, name, strlen(name));
    schedule(at, ev, 2 + 255);
}
static void inquiry_rssi(uint64_t at, uint8_t a0, int8_t rssi)
{
    uint8_t ev[2 + 15] = { 0x22, 15, 1, a0, 1, 2, 3, 4, 5, 1, 0, 0x0c, 0x02, 0x5a, 0x00, 0x00, (uint8_t)rssi };
    schedule(at, ev, sizeof(ev));
}

static void c_power_check(void)
{
    int p = chip_powered();
    if (p && !c_was_powered) {
        c_patched = c_download = 0;
        c_baud = 115200;
        c_have = 0;
        nsch = 0;
        c_powerups++;
    }
    if (!p && c_was_powered)
        nsch = 0;
    c_was_powered = p;
}

static void c_command(void)
{
    uint16_t op = (uint16_t)(c_cmd[1] | c_cmd[2] << 8);
    const uint8_t *p = c_cmd + 4;
    uint8_t n = c_cmd[3];
    if (c_mode == M_SILENT)
        return;
    switch (op) {
    case 0x0C03:
        c_resets++;
        c_cc(op, 0, 0);
        break;
    case 0x1001: {
        uint16_t sub = c_patched ? 0x2209 : 0x4105, rev = c_patched ? 0x0123 : 0x0000;
        uint8_t r[8] = { 9, (uint8_t)rev, (uint8_t)(rev >> 8), 9, 0x0f, 0x00, (uint8_t)sub, (uint8_t)(sub >> 8) };
        c_cc(op, r, 8);
        break;
    }
    case 0xFC2E:
        c_download = 1;
        c_rec_idx = 0;
        c_downloads++;
        c_cc(op, 0, 0);
        break;
    case 0xFC4C:
    case 0xFC4E: {
        /* the record must be the file's next one, byte for byte */
        uint32_t off = 0;
        struct bthci_rec r;
        int k = 0, ok = 0;
        while (bthci_hcd_next(hcd, hcd_len, &off, &r) == 1) {
            if (k++ == c_rec_idx) {
                ok = c_download && r.op == op && r.n == n && !memcmp(r.p, p, n);
                break;
            }
        }
        if (!ok)
            c_rec_bad++;
        c_rec_idx++;
        if (op == 0xFC4C && c_mode == M_REFUSE_RECORD && c_rec_idx == 5) {
            uint8_t ev[6] = { 0x0E, 4, 1, 0x4C, 0xFC, 0x0C };   /* status 0x0C: disallowed */
            c_event(ev, 6, T + 50);
            break;
        }
        if (op == 0xFC4E) {
            if (c_mode != M_LAUNCH_SILENT)
                c_cc(op, 0, 0);
            c_patched = 1;
            c_download = 0;
        } else {
            c_cc(op, 0, 0);
        }
        break;
    }
    case 0xFC18: {
        uint32_t b = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
        c_cc(op, 0, 0);
        c_baud = c_mode == M_BAUDFAIL ? b + b / 5 : b;
        break;
    }
    case 0x1009: {
        uint8_t a[6] = { 0x9a, 0x78, 0x56, 0x34, 0x12, 0x02 };
        if (c_default_addr)
            a[3] = 0xB2, a[4] = 0x61, a[5] = 0x43;
        c_cc(op, a, 6);
        break;
    }
    case 0x200C:
        c_cc(op, 0, 0);
        if (p[0]) {
            le_report(T + 100000, 0xA1, -60, "Kettle", 0);
            le_report(T + 200000, 0xA2, -40, "Watch", 1);
            le_report(T + 300000, 0xA3, -80, 0, 0);
            le_report(T + 400000, 0xA1, -50, "Kettle", 0);       /* the same device, stronger */
        }
        break;
    case 0x0401:
        c_cs(op, 0);
        inquiry_eir(T + 500000, 0xB1, -55, "Speaker");
        inquiry_rssi(T + 800000, 0xB2, -70);
        {
            uint8_t done[3] = { 0x01, 1, 0 };
            schedule(T + 1500000, done, 3);
        }
        break;
    default:
        c_cc(op, 0, 0);
        break;
    }
}

static void c_byte(uint8_t b)
{
    if (c_have == 0 && b != 1)
        return;
    c_cmd[c_have++] = b;
    if (c_have >= 4 && c_have == 4U + c_cmd[3]) {
        c_command();
        c_have = 0;
    }
}

/* due events into the UART's FIFO */
static void c_tick(void)
{
    c_power_check();
    for (unsigned i = 0; i < nsch;) {
        if (sch[i].t <= T) {
            c_send(sch[i].b, sch[i].n, T);
            sch[i] = sch[--nsch];
        } else {
            i++;
        }
    }
}

static unsigned rx_avail(void)
{
    unsigned n = 0;
    for (unsigned i = rx_head; i != rx_tail && n < 256; i++)
        if (rxq[i % 8192].t <= T)
            n++;
        else
            break;
    return n;
}

/* ---- the register bus ---- */
static uint32_t f_rd(uint64_t a)
{
    T++;
    if (a >= BTU_BASE && a < BTU_BASE + 0x100) {
        unsigned off = (unsigned)(a - BTU_BASE);
        c_tick();
        switch (off) {
        case BTU_UFSTAT: {
            unsigned n = rx_avail();
            return n >= 256 ? 0x100 : n;
        }
        case BTU_URXH: {
            if (rx_avail()) {
                uint8_t b = rxq[rx_head % 8192].b;
                rx_head++;
                return b;
            }
            return 0;
        }
        case BTU_UTRSTAT:
            return 6;
        case BTU_UMSTAT:
            return chip_powered() ? 1 : 0;
        case BTU_UERSTAT:
            return 0;
        default:
            return reg(a);
        }
    }
    return reg(a);
}

static int f_probe(uint64_t a, uint32_t *v)
{
    *v = f_rd(a);
    return 1;
}

static int write_ok(uint64_t a)
{
    if ((a >= BTU_BASE && a < BTU_BASE + 0x100) || (a >= 0x10930000ULL && a < 0x10930100ULL))
        return 1;
    return a == GPQ0 || a == GPQ0 + 8 || a == GPG2 || a == GPG2 + 4 || a == USI10 || a == GPP6 || a == GPP6 + 8 ||
           a == GPG3 || a == GPG3 + 4 || (a >= CMU1 + 0x2000 && a < CMU1 + 0x3100);
}

static void f_wr(uint64_t a, uint32_t v)
{
    writes++;
    if (!write_ok(a) && !bad_write)
        bad_write = a;
    if (a == GPQ0 && ((reg(a) ^ v) & 0xffff0000U) && !bad_write)
        bad_write = a;                      /* SPEEDY's pins */
    if (a >= BTU_BASE && a < BTU_BASE + 0x100) {
        unsigned off = (unsigned)(a - BTU_BASE);
        c_tick();
        int in_reset = reg(BTU_BASE + BTU_USI_CON) & 1;
        if (off == BTU_UTXH) {
            if (in_reset || !uart_clock_on)
                return;
            uint32_t div = 16 * (reg(BTU_BASE + BTU_UBRDIV) + 1) + reg(BTU_BASE + BTU_UFRACVAL);
            uint64_t byte_us = (uint64_t)10 * div * 1000000 / uart_clk;
            uint64_t start = tx_done > T ? tx_done : T;
            tx_done = start + byte_us;
            if (reg(BTU_BASE + BTU_UCON) & BTU_UCON_LOOP) {
                rx_push(tx_done, (uint8_t)v);
            } else if (chip_powered() && (reg(GPQ0) & 0xffff) == 0x2222) {
                if (rates_match())
                    c_byte((uint8_t)v);
            }
            return;
        }
        if (off == BTU_UFCON && (v & 2)) {
            rx_head = rx_tail;
            v &= ~6U;
        }
        set(a, v);
        return;
    }
    set(a, v);
    if (a == GPG3 || a == GPG3 + 4)
        x_check_reset();
    c_power_check();
}

/* ---- files ---- */
static uint8_t *vendor_file, *card_file;
static uint32_t vendor_len, card_len;
static const char *vendor_name = "firmware/bcm4361B2_semco.hcd";
static long f_vendor(const char *path, void *dst, uint32_t cap)
{
    T += 20000;
    if (!vendor_file || strcmp(path, vendor_name) || vendor_len > cap)
        return -1;
    memcpy(dst, vendor_file, vendor_len);
    return vendor_len;
}
static long f_card(const char *path, void *dst, uint32_t cap)
{
    if (!card_file || strcmp(path, "/STARBITE/FW/bcm4361B2_murata.hcd") || card_len > cap)
        return -22;
    memcpy(dst, card_file, card_len);
    return card_len;
}
static int f_pmic(uint32_t type, uint32_t r, uint8_t *v)
{
    *v = type == 1 && r == 0x13 ? 0x07 : 0;
    return 1;
}
static void *f_alloc(uint32_t n) { return calloc(1, n); }

/* a synthetic patch: 40 Write_RAM records (one with a description) and Launch_RAM */
static uint8_t *make_hcd(uint32_t *len, int corrupt)
{
    uint8_t *b = malloc(64 * 1024);
    uint32_t n = 0;
    for (unsigned r = 0; r < 40; r++) {
        uint32_t addr = 0x00210000 + r * 200;
        b[n++] = 0x4C, b[n++] = 0xFC, b[n++] = 4 + 200;
        b[n++] = (uint8_t)addr, b[n++] = (uint8_t)(addr >> 8), b[n++] = (uint8_t)(addr >> 16), b[n++] = 0;
        for (unsigned i = 0; i < 200; i++)
            b[n++] = (uint8_t)(r * 7 + i);
        if (r == 3)
            memcpy(b + n - 150, "BCM4361B2 synthetic test patch", 31);   /* with its 0 */
    }
    if (corrupt == 1) {                     /* a record that is not a vendor command */
        b[n++] = 0x03, b[n++] = 0x0C, b[n++] = 0;
    }
    b[n++] = 0x4E, b[n++] = 0xFC, b[n++] = 4;
    b[n++] = 0xff, b[n++] = 0xff, b[n++] = 0xff, b[n++] = 0xff;
    if (corrupt == 2)
        n -= 2;                             /* ends inside the last record */
    *len = n;
    return b;
}

/* ---- the phone as found ---- */
static const uint64_t watch[] = {
    GPQ0, GPQ0 + 8, GPG2, GPG2 + 4, GPG0, GPG0 + 4, GPG3, GPG3 + 4, GPP6, GPP6 + 8, USI10,
    BTU_BASE + BTU_ULCON, BTU_BASE + BTU_UCON, BTU_BASE + BTU_UMCON, BTU_BASE + BTU_UBRDIV, BTU_BASE + BTU_UFRACVAL,
    BTU_BASE + BTU_UINTM, BTU_BASE + BTU_USI_CON, BTU_BASE + BTU_USI_OPTION,
    0x10930000, 0x10930040, 0x109300C4, 0x10930060, 0x10930068,
    CMU1 + 0x2034, CMU1 + 0x201C, CMU1 + 0x20C4, CMU1 + 0x20C8, CMU1 + 0x3028,
};
#define NW (sizeof(watch) / sizeof(watch[0]))
static uint32_t snap[NW];

static struct bt_hal hal;
static uint8_t *the_hcd;
static uint32_t the_hcd_len;

static void phone(int expander_in_reset, int wifi_on)
{
    nreg = 0;
    T = 1000000;
    bad_write = 0;
    writes = 0;
    rx_head = rx_tail = 0;
    tx_done = 0;
    nsch = 0;
    c_mode = M_NORMAL;
    c_was_powered = 0;
    c_rec_bad = c_downloads = c_resets = c_rec_idx = 0;
    bt_fw_forget();
    c_powerups = 0;
    c_default_addr = 0;
    uart_clock_on = 1;
    uart_clk = 200000000;
    x_bus_up = 0;
    x_reset_prev = -1;
    x_other_changed = 0;
    /* as S-Boot leaves things (plausibly) */
    set(GPQ0, 0x00110000), set(GPQ0 + 8, 0x00005555);         /* gpq0-4/5 SPEEDY's: function 1 */
    set(GPG2, 0x00010000), set(GPG2 + 4, 0);                   /* gpg2-4 the torch's output */
    set(GPA2, 0), set(GPA2 + 4, 0x08);
    set(GPG0, wifi_on ? 0x00100000 : 0), set(GPG0 + 4, wifi_on ? 0x20 : 0);
    set(GPG3, expander_in_reset ? 0 : 0x01000000), set(GPG3 + 4, expander_in_reset ? 0 : 0x40);
    set(GPP6, 0x00000022), set(GPP6 + 8, 0);
    set(USI10, 4);
    set(BTU_BASE + BTU_USI_CON, 1);
    set(0x109300C4, 1);
    set(CMU1 + 0x0120, 0x10);
    x_out[0] = 0x12, x_out[1] = 0x01, x_out[2] = 0x00;
    x_cfg[0] = 0xED, x_cfg[1] = 0xFE, x_cfg[2] = 0xFF;
    x_other_in[0] = 0x21, x_other_in[1] = 0x00, x_other_in[2] = 0x40;
    if (expander_in_reset)
        memset(x_out, 0, 3), memset(x_cfg, 0xff, 3);
    memcpy(x_out0, x_out, 3);
    memcpy(x_cfg0, x_cfg, 3);
    x_check_reset();
    for (unsigned i = 0; i < NW; i++)
        snap[i] = reg(watch[i]);
    vendor_file = the_hcd;
    vendor_len = the_hcd_len;
    vendor_name = "firmware/bcm4361B2_semco.hcd";
    card_file = 0;
    hcd = the_hcd;
    hcd_len = the_hcd_len;
    log_clear();
}

static int as_found(void)
{
    int same = 1;
    for (unsigned i = 0; i < NW; i++)
        if (reg(watch[i]) != snap[i]) {
            printf("  register %llx: %08x, was %08x\n", (unsigned long long)watch[i], reg(watch[i]), snap[i]);
            same = 0;
        }
    if (memcmp(x_out, x_out0, 3) || memcmp(x_cfg, x_cfg0, 3)) {
        printf("  the expander's ports are not as found\n");
        same = 0;
    }
    return same && !chip_powered() && !x_other_changed && !bad_write;
}

static int run(int level, uint64_t limit_us)
{
    unsigned w = 0;
    uint64_t t0 = T;
    while (!drv_bt_probe(level, &w)) {
        T += (uint64_t)w * 1000;
        if (T - t0 > limit_us)
            return 0;
    }
    return 1;
}

static int last_line(const char *prefix)
{
    const char *p = strstr(logbuf, "drvprobe: bt: PASS");
    const char *f = strstr(logbuf, "drvprobe: bt: FAIL");
    return prefix[0] == 'P' ? p && !f : f && !p;
}

/* ---- the pure parts ---- */
static void test_hci(void)
{
    struct bthci_rx r;
    memset(&r, 0, sizeof(r));
    bthci_rx_reset(&r);
    const uint8_t in[] = { 0x00, 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0xaa, 0xbb,
                           0x04, 0x0F, 0x04, 0x00, 0x01, 0x01, 0x04 };
    int events = 0, skipped = 0;
    for (unsigned i = 0; i < sizeof(in); i++) {
        int k = bthci_rx_byte(&r, in[i]);
        if (k == BTHCI_RX_EVENT) {
            events++;
            uint16_t op;
            uint8_t s, rl;
            const uint8_t *ret;
            if (events == 1)
                CHECK(bthci_cmd_complete(r.ev, &op, &s, &ret, &rl) && op == 0x0C03 && s == 0 && rl == 0,
                      "Command Complete of HCI_Reset");
            else
                CHECK(bthci_cmd_status(r.ev, &op, &s) && op == 0x0401 && s == 0, "Command Status of Inquiry");
        }
        if (k == BTHCI_RX_SKIPPED)
            skipped++;
    }
    CHECK(events == 2 && skipped == 1 && r.junk == 1, "H4 framing: two events, one ACL skipped, one junk byte");

    uint8_t pkt[16];
    uint8_t p2[2] = { 1, 1 };
    CHECK(bthci_cmd(pkt, sizeof(pkt), 0x200C, p2, 2) == 6 && pkt[0] == 1 && pkt[1] == 0x0C && pkt[2] == 0x20 &&
              pkt[3] == 2, "a command's H4 bytes");
    CHECK(bthci_cmd(pkt, 4, 0x200C, p2, 2) == 0, "a command that does not fit");

    struct bthci_version v;
    const uint8_t vr[8] = { 9, 0x23, 0x01, 9, 0x0f, 0, 0x09, 0x22 };
    CHECK(bthci_version(vr, 8, &v) && v.hci_rev == 0x0123 && v.manufacturer == 15 && v.lmp_subver == 0x2209,
          "Read Local Version's fields");

    struct bthci_div d;
    CHECK(bthci_uart_div(200000000, 115200, &d) && d.ubrdiv == 107 && d.frac == 8, "115200 from 200 MHz");
    CHECK(bthci_uart_div(200000000, 4000000, &d) && d.ubrdiv == 2 && d.frac == 2 && d.actual == 4000000,
          "4 Mbaud from 200 MHz (UBRDIV 2, UFRACVAL 2, as an earlier port saw)");
    CHECK(!bthci_uart_div(26000000, 3000000, &d), "no 3 Mbaud from 26 MHz");
    CHECK(bthci_pick_baud(200000000, 3000000) == 3000000, "3 Mbaud picked from 200 MHz");
    CHECK(bthci_pick_baud(26000000, 3000000) == 1000000, "1 Mbaud picked from 26 MHz");

    uint32_t len;
    struct bthci_hcd info;
    uint8_t *h = make_hcd(&len, 0);
    CHECK(bthci_hcd_check(h, len, &info) == BTHCI_HCD_OK && info.records == 41 && info.writes == 40 &&
              info.launch == 0xffffffffU && !strcmp(info.name, "BCM4361B2 synthetic test patch"),
          "a good .hcd: 40 writes, Launch_RAM, its description");
    free(h);
    h = make_hcd(&len, 1);
    CHECK(bthci_hcd_check(h, len, &info) == BTHCI_HCD_NOTVENDOR, "a .hcd with a non-vendor record is refused");
    free(h);
    h = make_hcd(&len, 2);
    CHECK(bthci_hcd_check(h, len, &info) == BTHCI_HCD_TRUNC, "a truncated .hcd is refused");
    free(h);
    {
        const uint8_t noLaunch[] = { 0x4C, 0xFC, 5, 0, 0, 0x21, 0, 0xAA };
        const uint8_t early[] = { 0x4E, 0xFC, 4, 0xff, 0xff, 0xff, 0xff, 0x4C, 0xFC, 4, 0, 0, 0, 0 };
        CHECK(bthci_hcd_check(noLaunch, sizeof(noLaunch), &info) == BTHCI_HCD_NOLAUNCH, "no Launch_RAM: refused");
        CHECK(bthci_hcd_check(early, sizeof(early), &info) == BTHCI_HCD_EARLYLAUNCH, "an early Launch_RAM: refused");
        CHECK(bthci_hcd_check(early, 0, &info) == BTHCI_HCD_EMPTY, "an empty file: refused");
    }

    char nm[33];
    const uint8_t ad[] = { 2, 1, 6, 4, 0x08, 'S', 'h', 'o', 6, 0x09, 'L', 'o', 'n', 'g', '!' };
    bthci_ad_name(ad, sizeof(ad), nm, sizeof(nm));
    CHECK(!strcmp(nm, "Long!"), "AD: the complete name before the shortened one");
    bthci_ad_name(ad, 8, nm, sizeof(nm));
    CHECK(!strcmp(nm, "Sho"), "AD: the shortened name when it is the only one");
    const uint8_t bad[] = { 9, 0x09, 'x' };
    bthci_ad_name(bad, sizeof(bad), nm, sizeof(nm));
    CHECK(nm[0] == 0, "AD: a field longer than the data is ignored");

    static struct bthci_devs l;
    bthci_devs_clear(&l);
    const uint8_t le[] = { 0x3E, 21, 0x02, 1, 0, 1, 1, 2, 3, 4, 5, 6, 9, 2, 1, 6, 5, 9, 'B', 'u', 'd', 's', 0xC4 };
    CHECK(bthci_scan_event(&l, le) && l.n == 1 && l.d[0].le && l.d[0].rssi == -60 && !strcmp(l.d[0].name, "Buds"),
          "an LE advertising report with its AD name and RSSI");
    const uint8_t ir[] = { 0x22, 15, 1, 9, 9, 9, 9, 9, 9, 1, 0, 0x0c, 0x02, 0x5a, 0, 0, (uint8_t)-30 };
    CHECK(bthci_scan_event(&l, ir) && l.n == 2 && !l.d[1].le && l.d[1].rssi == -30 && l.d[1].cod == 0x5a020c,
          "an inquiry result with RSSI and its class");
    unsigned idx[4];
    CHECK(bthci_devs_sorted(&l, idx, 4) == 2 && idx[0] == 1 && idx[1] == 0, "the list strongest first");
    const uint8_t done[] = { 0x01, 1, 0 };
    CHECK(bthci_scan_event(&l, done) && l.inquiry_done, "Inquiry Complete");

    /* pairing groundwork: Just Works */
    const uint8_t ioreq[] = { 0x31, 6, 1, 2, 3, 4, 5, 6 };
    const uint8_t conf[] = { 0x33, 10, 1, 2, 3, 4, 5, 6, 0x40, 0xE2, 0x01, 0x00 };
    uint8_t out[32];
    unsigned n = bthci_ssp_answer(ioreq, out, sizeof(out));
    CHECK(n == 13 && out[1] == 0x2B && out[2] == 0x04 && out[3] == 9 && out[4] == 1 && out[10] == 3 && out[11] == 0,
          "SSP: IO Capability Request -> Reply, NoInputNoOutput, no OOB");
    n = bthci_ssp_answer(conf, out, sizeof(out));
    CHECK(n == 10 && out[1] == 0x2C && out[2] == 0x04 && out[4] == 1 && out[9] == 6,
          "SSP: User Confirmation Request -> Reply (Just Works)");
    CHECK(bthci_ssp_answer(done, out, sizeof(out)) == 0, "SSP: other events get no answer");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_hci();

    the_hcd = make_hcd(&the_hcd_len, 0);
    hal.io.rd32 = f_rd;
    hal.io.probe32 = f_probe;
    hal.io.wr32 = f_wr;
    hal.io.i2c_init = f_i2c_init;
    hal.io.i2c_rd = f_i2c_rd;
    hal.io.i2c_wr = f_i2c_wr;
    hal.io.delay_us = f_delay;
    hal.io.log = f_log;
    hal.now_us = f_now;
    hal.vendor_read = f_vendor;
    hal.card_read = f_card;
    hal.pmic_read = f_pmic;
    hal.alloc = f_alloc;
    bt_hal_set(&hal);

    /* level 0: read only */
    phone(0, 1);
    CHECK(run(0, 1000000) && last_line("PASS") && writes == 0, "level 0 PASS, no write at all");
    CHECK(logged("Wi-Fi powered") && logged("RTCBUF (0x13, the 32 kHz buffers) 07") &&
              logged("firmware VENDOR firmware/bcm4361B2_semco.hcd") && logged("synthetic test patch"),
          "level 0 logs the pins, the 32 kHz buffers and the firmware");

    /* level 1 */
    phone(0, 1);
    CHECK(run(1, 60000000) && last_line("PASS"), "level 1 PASS");
    CHECK(c_rec_bad == 0 && c_downloads == 1 && c_rec_idx == 41, "every .hcd record sent once, in order");
    CHECK(bt_info()->baud == 3000000 && bt_info()->uart_clk_hz == 200000000, "3 Mbaud from a measured 200 MHz");
    CHECK(bt_info()->rom.lmp_subver == 0x4105 && bt_info()->patched.lmp_subver == 0x2209, "ROM and patched versions");
    CHECK(as_found(), "level 1: everything back as found, the chip off, only BT_REG_ON's bits touched");
    CHECK(logged("address read: the chip's own"), "the address is read");
    {
        /* the address never reaches the log in any form */
        const char *forms[] = { "02:12:34:56:78:9a", "02:12:34", "9a:78", "021234", "9a7856" };
        int leaked = 0;
        for (unsigned i = 0; i < 5; i++)
            if (strstr(logbuf, forms[i]))
                leaked = 1;
        CHECK(!leaked, "the address is never logged");
    }
    CHECK(logged("drvprobe: bt:   LE -40 dBm") && logged("\"Watch\"") && logged("Kettle"),
          "the strongest devices with their names");
    CHECK(bt_ndev() == 5 && bt_dev_at(0)->rssi == -40 && !strcmp(bt_dev_at(1)->name, "Kettle") &&
              bt_dev_at(1)->rssi == -50, "the device list: 3 LE + 2 classic, strongest first, a name from EIR");
    {
        int speaker = 0;
        for (unsigned i = 0; i < bt_ndev(); i++)
            if (!strcmp(bt_dev_at(i)->name, "Speaker") && !bt_dev_at(i)->le)
                speaker = 1;
        CHECK(speaker, "the inquiry's EIR name");
    }
    CHECK(reg(GPG0) == 0x00100000 && reg(GPG0 + 4) == 0x20, "Wi-Fi's WLAN_REG_ON untouched");

    /* the default address */
    phone(0, 0);
    c_default_addr = 1;
    CHECK(run(1, 60000000) && last_line("PASS") && logged("Broadcom's default"), "Broadcom's default address said");

    /* a controller that never answers */
    phone(0, 0);
    c_mode = M_SILENT;
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("does not answer (HCI_Reset after power-on)"),
          "a silent chip: FAIL in plain words");
    CHECK(as_found() && bt_state() == BT_OFF, "a silent chip: off and everything back");

    /* a corrupt .hcd: refused before the chip is powered */
    {
        uint32_t len;
        uint8_t *bad = make_hcd(&len, 1);
        phone(0, 0);
        vendor_file = bad;
        vendor_len = len;
        CHECK(run(1, 60000000) && last_line("FAIL") && logged("damaged (refused before sending)") &&
                  c_powerups == 0 && writes == 0, "a corrupt .hcd: refused, the chip never powered, nothing written");
        phone(0, 0);
        vendor_file = bad;
        vendor_len = len;
        CHECK(run(0, 1000000) && last_line("FAIL"), "a corrupt .hcd: level 0 FAIL");
        free(bad);
    }

    /* no firmware on VENDOR, the card has it */
    phone(0, 0);
    vendor_file = 0;
    card_file = the_hcd;
    card_len = the_hcd_len;
    CHECK(run(0, 1000000) && last_line("PASS") && logged("card /STARBITE/FW/bcm4361B2_murata.hcd"),
          "the firmware from the card");
    phone(0, 0);
    vendor_file = 0;
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("firmware is missing") && writes == 0,
          "no firmware anywhere: FAIL, nothing written");

    /* the fast rate does not work: off, on, patched again, kept at 115200 */
    phone(0, 0);
    c_mode = M_BAUDFAIL;
    CHECK(run(1, 60000000) && last_line("PASS") && bt_info()->baud == 115200 && c_downloads == 2 && c_rec_bad == 0,
          "a failed fast rate: patched again, PASS at 115200");
    CHECK(as_found(), "after the fallback: everything back");

    /* Launch_RAM unanswered: going on */
    phone(0, 0);
    c_mode = M_LAUNCH_SILENT;
    CHECK(run(1, 60000000) && last_line("PASS") && logged("Launch_RAM unanswered"), "Launch_RAM unanswered: PASS");

    /* a record refused */
    phone(0, 0);
    c_mode = M_REFUSE_RECORD;
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("did not take its patch") && as_found(),
          "a refused record: FAIL, off, everything back");

    /* the expander held in reset: released, then put back */
    phone(1, 0);
    CHECK(run(1, 60000000) && last_line("PASS") && logged("held in reset") && as_found(),
          "the expander held in reset: released for the run, put back");

    /* Bluetooth already powered by someone else */
    phone(0, 0);
    x_out[0] |= 0x80;
    x_cfg[0] &= 0x7f;
    memcpy(x_out0, x_out, 3);
    memcpy(x_cfg0, x_cfg, 3);
    c_power_check();
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("already powered by someone else") &&
              (x_out[0] & 0x80) && !(x_cfg[0] & 0x80), "BT already on: refused, BT_REG_ON left high");

    /* UART_BT's clock gate forced off */
    phone(0, 0);
    set(CMU1 + 0x20C4, 0x00100000);
    for (unsigned i = 0; i < NW; i++)
        snap[i] = reg(watch[i]);
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("clock gate is closed") && c_powerups == 0 && as_found(),
          "a closed gate: FAIL, not opened");

    /* UART_BT's clock not running */
    phone(0, 0);
    uart_clock_on = 0;
    CHECK(run(1, 60000000) && last_line("FAIL") && logged("clock does not run") && c_powerups == 0 && as_found(),
          "a stopped UART clock: FAIL before the chip is powered, everything back");

    /* a 26 MHz UART clock: 1 Mbaud */
    phone(0, 0);
    uart_clk = 26000000;
    CHECK(run(1, 60000000) && last_line("PASS") && bt_info()->uart_clk_hz == 26000000 && bt_info()->baud == 1000000,
          "a 26 MHz UART clock: measured, 1 Mbaud");

    /* the watchdog's abort in the middle of the patch */
    phone(0, 0);
    {
        unsigned w;
        int k = 0;
        while (!drv_bt_probe(1, &w) && c_rec_idx < 10 && k++ < 100000)
            T += (uint64_t)w * 1000;
        CHECK(c_rec_idx >= 10 && chip_powered(), "mid-patch");
        drv_bt_abort();
        CHECK(as_found() && bt_state() == BT_OFF, "abort mid-patch: off, everything back");
    }

    /* the budget */
    phone(0, 0);
    {
        unsigned w;
        CHECK(!drv_bt_probe(1, &w), "first step");
        T += 50000000ULL;
        CHECK(drv_bt_probe(1, &w) == 1 && last_line("FAIL") && logged("within its budget") && as_found(),
              "over budget: FAIL, everything back");
    }

    /* level 2: the long list */
    phone(0, 0);
    CHECK(run(2, 80000000) && last_line("PASS") && logged("\"Speaker\"") && logged("(no name)") && as_found(),
          "level 2 PASS with every device listed");

    /* the kernel API: on, scan, off */
    phone(0, 0);
    bt_on();
    {
        unsigned k = 0;
        while (!bt_step() && k++ < 1000000)
            T += (uint64_t)bt_wait_ms() * 1000;
        CHECK(bt_state() == BT_READY && !strcmp(bt_why(), "Bluetooth is on"), "bt_on: ready");
        CHECK(bt_scan(2000, 2000) == 0, "bt_scan starts");
        while (!bt_step() && k++ < 1000000)
            T += (uint64_t)bt_wait_ms() * 1000;
        CHECK(bt_state() == BT_READY && bt_ndev() >= 4, "bt_scan: devices heard");
        bt_off();
        CHECK(bt_state() == BT_OFF && bt_restored() && as_found(), "bt_off: everything back");
    }

    printf("bt-test: %d checks, %d failed\n", checks, failures);
    free(the_hcd);
    return failures ? 1 : 0;
}
