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

/* ---- Pairing and HID keyboards (bt_pair.c, bt_l2cap.c, bt_hid.c; docs/drivers/BLUETOOTH.md) ----
 * One classic (BR/EDR) keyboard in boot protocol. Single-threaded: everything runs inside bt_step(); while
 * bt_poll_ms() > 0 call bt_step() again within that many ms even after it returned 1 (a pairing, a connected or a
 * paired keyboard waiting to come back). Nothing here logs an address, a link key or a typed character.
 *   bt_set_link_key_store(get, put)   once, before pairing. get: 1 and the 16-byte key if one is stored for addr.
 *                                     put: a new key after a successful pairing (store it with addr, e.g. in prefs),
 *                                     or key == 0: forget addr's key. Keys never go to the log, git or notes.
 *   bt_set_paired(addr)               at boot, the stored keyboard's address (0: none): page scan goes on when the chip
 *                                     is up, and only that address may connect (Accept_Connection_Request).
 *   bt_pair(i)                        pair bt_dev_at(i) (classic; class "peripheral / keyboard" or a keyboard's name).
 *                                     0 started (if a scan runs, it starts when the scan ends), -1 refused (bt_why()).
 *   bt_pair_state()                   BT_PAIR_*; bt_why() gives the plain words for it while the chip is on.
 *   bt_pair_passkey()                 while BT_PAIR_CODE: the number to show (SSP passkey or our legacy PIN), else -1;
 *                                     bt_pair_code() the same as 6 digits ("042195"), "" when none. The person types
 *                                     it on the keyboard, then Enter. bt_pair_digits(): digits typed so far (if the
 *                                     keyboard reports them), -1 unknown.
 *   bt_hid_connected()                1 while the keyboard's HID channels are up (keys arrive)
 *   bt_hid_set_key_cb(cb)             cb(usage, down, unicode): HID usage (page 7: 0x04 'a' .. 0x28 Enter, 0x4F..0x52
 *                                     arrows, 0xE0..0xE7 modifiers), 1 down / 0 up, the character (US layout, Shift and
 *                                     Caps Lock; '\n' Enter, 0x08 Backspace, '\t' Tab, 0x1B Esc, 0x7F Delete; 0 none).
 *                                     No key repeat (the UI repeats). bt_hid_mods(): the modifier byte (HID_MOD_*).
 *   bt_hid_reconnect()                page the paired keyboard with its stored key (if it does not come back itself)
 *   bt_unpair()                       disconnect (telling the keyboard: virtual cable unplug), forget the key (put(addr,
 *                                     0)), page scan off. */
enum { BT_PAIR_IDLE = 0, BT_PAIR_CONNECTING, BT_PAIR_CODE, BT_PAIR_SECURING, BT_PAIR_CONNECTED, BT_PAIR_WAITING,
       BT_PAIR_FAILED };
typedef int (*bt_key_get_fn)(const uint8_t addr[6], uint8_t key[16]);
typedef void (*bt_key_put_fn)(const uint8_t addr[6], const uint8_t *key16);
void bt_set_link_key_store(bt_key_get_fn get, bt_key_put_fn put);
void bt_set_paired(const uint8_t *addr6);
int bt_paired(uint8_t addr[6]);             /* 1 and the address if a keyboard is paired */
int bt_pair(int dev_index);
int bt_hid_reconnect(void);
int bt_pair_state(void);
int bt_pair_passkey(void);
const char *bt_pair_code(void);
int bt_pair_digits(void);
const char *bt_pair_why(void);              /* the pairing's own words (bt_why() shows them while the chip is on) */
void bt_unpair(void);
int bt_hid_connected(void);
void bt_hid_set_key_cb(void (*cb)(int usage, int down, uint32_t unicode));
int bt_hid_mods(void);
unsigned bt_poll_ms(void);
int bt_is_keyboard(const struct bt_dev *d); /* class peripheral + keyboard bit, or "keyboard" in the name */

/* the on-phone probe (drvprobe.h) */
int drv_bt_probe(int level, unsigned *wait_ms);
void drv_bt_abort(void);
#define DRV_BT_BUDGET_MS 130000U               /* level 2 (a keyboard: pairing <= 60 s + 20 s of typing); level 1
                                                  ends itself at 45 s */

/* the host test's view */
int bt_look(void);                          /* level 0: 1 PASS, 0 FAIL (logged) */
#endif
