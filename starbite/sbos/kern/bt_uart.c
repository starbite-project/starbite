/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: UART_BT, polled (docs/drivers/BLUETOOTH.md 2).
 *
 * uart@10840000 of the phone's DT (Samsung's "uart1", /dev/ttySAC1 to Android): samsung,usi-serial-v2, a 256-byte
 * FIFO, clocks CMU_PERIC1 gate_pclk1 / gate_uart1. Registers from Samsung's serial driver (include/linux/serial_s3c.h,
 * drivers/tty/serial/samsung.c, GPL-2.0): ULCON 0x00, UCON 0x04 (bit 5 loopback; [3:2], [1:0] TX / RX by the CPU),
 * UFCON 0x08 (bit 0 FIFO, bits 1-2 reset), UMCON 0x0C (bit 4 auto flow control), UTRSTAT 0x10 (bit 2 all sent),
 * UERSTAT 0x14, UFSTAT 0x18 (RX count [7:0], RX full 8, TX count [23:16], TX full 24), UMSTAT 0x1C (bit 0 CTS),
 * UTXH 0x20, URXH 0x24, UBRDIV 0x28, UFRACVAL 0x2C, UINTP/UINTSP/UINTM 0x30-0x38, USI_CON 0xC4 (bit 0 reset),
 * USI_OPTION 0xC8 (bit 1 CLKREQ on). The serial driver takes the block out of its USI reset (USI_CON 1 then 0) and
 * sets USI_OPTION 2; S-Boot leaves it in reset (an earlier port read USI_CON = 1 on this phone; facts only).
 * The input clock is not trusted: S-Boot may leave CMU_PERIC1's IP mux on the 26 MHz oscillator, Android sets
 * 200 MHz. bt_uart_measure() times 16 bytes in the internal loopback (no pin moves) and the divisor is worked out
 * from what it measured. CMU registers are only read here.
 * Seen on a phone: nothing of this file yet. */
#include "bt_uart.h"

static const uint16_t save_off[BTU_NSAVE] = {
    BTU_ULCON, BTU_UCON, BTU_UFCON, BTU_UMCON, BTU_UBRDIV, BTU_UFRACVAL, BTU_UINTM, BTU_USI_OPTION, BTU_USI_CON,
};

static uint32_t rd(struct bt_uart *u, unsigned off)
{
    return u->h->io.rd32(BTU_BASE + off);
}

static void wr(struct bt_uart *u, unsigned off, uint32_t v)
{
    if (off < 0x100)
        u->h->io.wr32(BTU_BASE + off, v);
}

int bt_uart_save(struct bt_uart *u, const struct bt_hal *h)
{
    u->h = h;
    u->saved_ok = 0;
    u->up = 0;
    u->rx_err = 0;
    for (unsigned i = 0; i < BTU_NSAVE; i++)
        if (!h->io.probe32(BTU_BASE + save_off[i], &u->saved[i]))
            return 0;
    u->saved_ok = 1;
    return 1;
}

void bt_uart_log(struct bt_uart *u, const char *when)
{
    static const char *const names[BTU_NSAVE] = {
        "ULCON", "UCON", "UFCON", "UMCON", "UBRDIV", "UFRACVAL", "UINTM", "USI_OPTION", "USI_CON",
    };
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: UART_BT ");
    btl_s(&l, when);
    btl_s(&l, ":");
    for (unsigned i = 0; i < BTU_NSAVE; i++) {
        uint32_t v = 0;
        btl_s(&l, " ");
        btl_s(&l, names[i]);
        btl_s(&l, " ");
        if (u->h->io.probe32(BTU_BASE + save_off[i], &v))
            btl_x(&l, v, i == 4 || i == 1 ? 4 : 2);
        else
            btl_s(&l, "??");
    }
    {
        uint32_t f = 0, m = 0;
        if (u->h->io.probe32(BTU_BASE + BTU_UFSTAT, &f) && u->h->io.probe32(BTU_BASE + BTU_UMSTAT, &m)) {
            btl_s(&l, " UFSTAT ");
            btl_x(&l, f, 8);
            btl_s(&l, " UMSTAT ");
            btl_x(&l, m, 2);
        }
    }
    u->h->io.log(l.b);
}

void bt_uart_flush(struct bt_uart *u)
{
    uint64_t t0 = u->h->now_us();
    wr(u, BTU_UFCON, BTU_UFCON_FIFO | BTU_UFCON_RESET);
    while ((rd(u, BTU_UFCON) & BTU_UFCON_RESET) && u->h->now_us() - t0 < 2000) {
    }
    wr(u, BTU_UFCON, BTU_UFCON_FIFO);
    (void)rd(u, BTU_UERSTAT);
}

void bt_uart_start(struct bt_uart *u)
{
    wr(u, BTU_USI_CON, 1);
    u->h->io.delay_us(10);
    wr(u, BTU_USI_CON, 0);
    u->h->io.delay_us(10);
    wr(u, BTU_USI_OPTION, 2);
    wr(u, BTU_UINTM, 0xf);
    wr(u, BTU_UINTP, 0xf);
    wr(u, BTU_UINTSP, 0xf);
    wr(u, BTU_UCON, BTU_UCON_POLL);
    wr(u, BTU_UMCON, 0);
    bt_uart_flush(u);
    wr(u, BTU_ULCON, 3);                    /* 8N1 */
    u->up = 1;
}

/* a measured clock near a known one is taken as that one: 26 MHz or CMU_TOP's 400 MHz, divided by 1..16 */
static uint32_t snap(uint32_t hz)
{
    static const uint32_t base[2] = { 26000000U, 400000000U };
    for (unsigned b = 0; b < 2; b++)
        for (uint32_t k = 1; k <= 16; k++) {
            uint32_t c = base[b] / k, e = hz > c ? hz - c : c - hz;
            if ((uint64_t)e * 100 <= c)
                return c;
        }
    return hz;
}

uint32_t bt_uart_measure(struct bt_uart *u)
{
    static const uint8_t pat[16] = { 0x55, 0xaa, 0x0f, 0xf0, 0x31, 0x13, 0x77, 0x00,
                                     0xff, 0x5a, 0xa5, 0x42, 0x24, 0x81, 0x18, 0x99 };
    const uint32_t div = 2048;              /* UBRDIV 127, UFRACVAL 0 */
    wr(u, BTU_UCON, BTU_UCON_LOOP | BTU_UCON_POLL);
    wr(u, BTU_UMCON, 0);
    wr(u, BTU_UBRDIV, div / 16 - 1);
    wr(u, BTU_UFRACVAL, 0);
    bt_uart_flush(u);
    uint64_t t0 = u->h->now_us(), t1 = t0;
    for (unsigned i = 0; i < sizeof(pat); i++)
        wr(u, BTU_UTXH, pat[i]);
    unsigned n = 0;
    while (u->h->now_us() - t0 < 40000) {
        n = rd(u, BTU_UFSTAT) & BTU_UFSTAT_RXCNT;
        if (n >= sizeof(pat)) {
            t1 = u->h->now_us();
            break;
        }
    }
    int same = n >= sizeof(pat);
    for (unsigned i = 0; same && i < sizeof(pat); i++)
        if ((rd(u, BTU_URXH) & 0xff) != pat[i])
            same = 0;
    wr(u, BTU_UCON, BTU_UCON_POLL);
    bt_uart_flush(u);
    if (!same || t1 <= t0)
        return 0;
    uint64_t baud = (uint64_t)sizeof(pat) * 10U * 1000000U / (t1 - t0);
    u->clk_hz = snap((uint32_t)(baud * div));
    return u->clk_hz;
}

int bt_uart_baud(struct bt_uart *u, uint32_t baud)
{
    struct bthci_div d;
    if (!bthci_uart_div(u->clk_hz, baud, &d))
        return 0;
    uint64_t t0 = u->h->now_us();
    while (!(rd(u, BTU_UTRSTAT) & BTU_UTRSTAT_TXEMPTY) && u->h->now_us() - t0 < 20000) {
    }
    wr(u, BTU_UBRDIV, d.ubrdiv);
    wr(u, BTU_UFRACVAL, d.frac);
    u->baud = baud;
    return 1;
}

void bt_uart_afc(struct bt_uart *u, int on)
{
    wr(u, BTU_UMCON, on ? BTU_UMCON_AFC : 0);
}

int bt_uart_write(struct bt_uart *u, const uint8_t *p, unsigned n, uint32_t timeout_us)
{
    uint64_t t0 = u->h->now_us();
    for (unsigned i = 0; i < n; i++) {
        while (rd(u, BTU_UFSTAT) & BTU_UFSTAT_TXFULL)
            if (u->h->now_us() - t0 > timeout_us)
                return 0;
        wr(u, BTU_UTXH, p[i]);
    }
    return 1;
}

int bt_uart_getc(struct bt_uart *u)
{
    uint32_t st = rd(u, BTU_UFSTAT);
    if (!(st & BTU_UFSTAT_RXCNT) && !(st & BTU_UFSTAT_RXFULL))
        return -1;
    if (rd(u, BTU_UERSTAT) & 0xf)
        u->rx_err++;
    return (int)(rd(u, BTU_URXH) & 0xff);
}

int bt_uart_cts(struct bt_uart *u)
{
    return (int)(rd(u, BTU_UMSTAT) & BTU_UMSTAT_CTS);
}

void bt_uart_restore(struct bt_uart *u)
{
    if (!u->saved_ok)
        return;
    wr(u, BTU_UMCON, 0);                    /* flow control off first */
    bt_uart_flush(u);
    for (unsigned i = 0; i < BTU_NSAVE; i++) {
        uint32_t v = u->saved[i];
        if (save_off[i] == BTU_UFCON)
            v &= ~BTU_UFCON_RESET;
        wr(u, save_off[i], v);
    }
    u->up = 0;
}

int bt_uart_same(struct bt_uart *u)
{
    if (!u->saved_ok)
        return 1;
    for (unsigned i = 0; i < BTU_NSAVE; i++) {
        uint32_t v = 0, w = u->saved[i];
        if (save_off[i] == BTU_UFCON)
            w &= ~BTU_UFCON_RESET;
        if (!u->h->io.probe32(BTU_BASE + save_off[i], &v))
            return 0;
        if (save_off[i] == BTU_UFCON)
            v &= ~BTU_UFCON_RESET;
        if (v != w)
            return 0;
    }
    return 1;
}
