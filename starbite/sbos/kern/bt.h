/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: Bluetooth on the BCM4361's Bluetooth half (bt.c; docs/drivers/BLUETOOTH.md). HCI over H4 on UART_BT
 * (bt_uart.c), BT_REG_ON on the PCAL6524 expander (pcal6524.c), Broadcom's patch from VENDOR or the card, the host
 * side in bt_hci.c. No host stack beyond scanning yet.
 *
 * The kernel's interface (single-threaded, no locks; nothing blocks for more than ~20 ms):
 *   bt_hal_libsb(&h); bt_hal_set(&h);      once
 *   bt_on();                               start: firmware read and checked, chip powered, patched, fast rate
 *   bt_step();                             every bt_wait_ms() while it returns 0 (the kernel's timer)
 *   bt_scan(le_ms, inquiry_ms);            when bt_state() == BT_READY: an LE scan, then a classic inquiry
 *   bt_ndev() / bt_dev_at(i)               the devices heard, strongest first (names, RSSI; addresses for the UI only)
 *   bt_off();                              off: the chip unpowered, every pin / register back as found
 *   bt_state(), bt_why(), bt_info()        for the UI: bt_why() is plain words
 * The address (bt_info()->addr) is the controller's own, read with Read_BD_ADDR; it is never logged or stored.
 */
#ifndef SBOS_BT_H
#define SBOS_BT_H
#include <stdint.h>
#include "bt_hci.h"
#include "pcal6524.h"

struct bt_hal {
    struct pcal_io io;                      /* registers, the expander's I2C, delays, the log */
    uint64_t (*now_us)(void);
    /* a file into dst: its length, or < 0 (missing, unreadable, larger than cap). VENDOR paths are relative to its
     * root ("firmware/x.hcd"); card paths absolute ("/STARBITE/FW/x.hcd"). May be NULL. */
    long (*vendor_read)(const char *path, void *dst, uint32_t cap);
    long (*card_read)(const char *path, void *dst, uint32_t cap);
    int (*pmic_read)(uint32_t type, uint32_t reg, uint8_t *v);   /* S2MPS18 via ACPM, read only; may be NULL */
    void *(*alloc)(uint32_t n);             /* never freed */
};
void bt_hal_set(const struct bt_hal *h);
#ifdef SBOS_KERNEL
void bt_hal_libsb(struct bt_hal *h);        /* libsb + bcmw_sb_vendor_read + kfile_read */
#endif

enum { BT_OFF = 0, BT_STARTING, BT_READY, BT_SCANNING, BT_FAILED };

struct bt_info {
    struct bthci_version rom, patched, now;
    uint32_t uart_clk_hz, baud;
    char fw[48];                            /* the patch file used */
    char fw_name[48];                       /* its own description */
    uint32_t fw_records;
    uint8_t addr[6];                        /* the controller's; for the UI on the phone only */
    int addr_read, addr_default;            /* addr_default: Broadcom's unprogrammed 43:61:B2:.. (not unique) */
    uint32_t rx_errors, junk;
};

int bt_on(void);                            /* 0 started (or already on), -1 no HAL */
int bt_scan(unsigned le_ms, unsigned inquiry_ms);   /* 0 started; -1 not READY */
int bt_step(void);                          /* 0: call again after bt_wait_ms(); 1: settled (READY, OFF, FAILED) */
unsigned bt_wait_ms(void);
void bt_off(void);                          /* safe at any point; restores everything */
int bt_state(void);
const char *bt_why(void);                   /* plain words: "Bluetooth is on", "The Bluetooth firmware is missing", .. */
const struct bt_info *bt_info(void);
unsigned bt_ndev(void);
const struct bt_dev *bt_dev_at(unsigned i); /* strongest first */
int bt_restored(void);                      /* after bt_off(): 1 if every register read back as found */
void bt_set_module(int semco);              /* Wi-Fi's OTP module ID: 1 SEMCO, 0 Murata (default: SEMCO first) */
void bt_allow_clock_gates(int on);         /* opt-in: force UART_BT's / USI10_I2C's CMU gates on (restored) */
void bt_fw_forget(void);                    /* the next bt_on() / look reads the patch again (it is kept once read) */

/* the on-phone probe (drvprobe.h) */
int drv_bt_probe(int level, unsigned *wait_ms);
void drv_bt_abort(void);
#define DRV_BT_BUDGET_MS 55000U                /* level 2; level 1 ends itself at 45 s */

/* the host test's view */
int bt_look(void);                          /* level 0: 1 PASS, 0 FAIL (logged) */
#endif
