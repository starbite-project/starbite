/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: UART_BT (0x10840000), the Exynos 9810 UART the BCM4361's Bluetooth half is on; polled (bt_uart.c). */
#ifndef SBOS_BT_UART_H
#define SBOS_BT_UART_H
#include <stdint.h>
#include "bt.h"

#define BTU_BASE 0x10840000ULL
#define BTU_ULCON 0x00
#define BTU_UCON 0x04
#define BTU_UFCON 0x08
#define BTU_UMCON 0x0C
#define BTU_UTRSTAT 0x10
#define BTU_UERSTAT 0x14
#define BTU_UFSTAT 0x18
#define BTU_UMSTAT 0x1C
#define BTU_UTXH 0x20
#define BTU_URXH 0x24
#define BTU_UBRDIV 0x28
#define BTU_UFRACVAL 0x2C
#define BTU_UINTP 0x30
#define BTU_UINTSP 0x34
#define BTU_UINTM 0x38
#define BTU_USI_CON 0xC4                    /* bit 0: the USI wrapper holds the UART in reset */
#define BTU_USI_OPTION 0xC8                 /* bit 1: CLKREQ on */
#define BTU_UCON_LOOP (1U << 5)
#define BTU_UCON_POLL 0x5U                  /* TX [3:2] and RX [1:0] by the CPU */
#define BTU_UFCON_FIFO 1U
#define BTU_UFCON_RESET 6U
#define BTU_UMCON_AFC (1U << 4)
#define BTU_UFSTAT_RXCNT 0xffU
#define BTU_UFSTAT_RXFULL (1U << 8)
#define BTU_UFSTAT_TXFULL (1U << 24)
#define BTU_UTRSTAT_TXEMPTY (1U << 2)
#define BTU_UMSTAT_CTS 1U
#define BTU_FIFO 256U

/* the registers that are put back (USI_CON last) */
#define BTU_NSAVE 9
struct bt_uart {
    const struct bt_hal *h;
    uint32_t clk_hz, baud, rx_err;
    uint32_t saved[BTU_NSAVE];
    int saved_ok, up;
};
int bt_uart_save(struct bt_uart *u, const struct bt_hal *h);   /* 0 if the block does not read */
void bt_uart_log(struct bt_uart *u, const char *when);
/* out of the USI reset, interrupts masked, FIFOs on and empty, 8N1, polled, no flow control */
void bt_uart_start(struct bt_uart *u);
/* the UART's input clock, measured in its internal loopback against the arch timer: Hz, 0 if nothing came back */
uint32_t bt_uart_measure(struct bt_uart *u);
int bt_uart_baud(struct bt_uart *u, uint32_t baud);    /* waits (<= 20 ms) for what is being sent; 0 no divisor */
void bt_uart_afc(struct bt_uart *u, int on);
void bt_uart_flush(struct bt_uart *u);
/* all n bytes into the FIFO within timeout_us: 1; 0 when the FIFO stays full (the chip holds CTS) */
int bt_uart_write(struct bt_uart *u, const uint8_t *p, unsigned n, uint32_t timeout_us);
int bt_uart_getc(struct bt_uart *u);        /* a byte or -1 */
int bt_uart_cts(struct bt_uart *u);
void bt_uart_restore(struct bt_uart *u);    /* every saved register as found, USI_CON last */
int bt_uart_same(struct bt_uart *u);        /* 1 if the saved registers read back as found */
#endif
