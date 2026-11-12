/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Bluetooth host side that touches no hardware (bt_hci.c), so the host test checks all of it.
 *   - H4 (HCI over a UART): one type byte, then the packet: 1 command (opcode le16, length u8, parameters), 2 ACL,
 *     3 SCO, 4 event (code u8, length u8, parameters). Bluetooth Core spec vol 4 part A; Linux hci.h.
 *   - Command Complete / Command Status, Read Local Version (Core vol 4 part E 7.7.14, 7.7.15, 7.4.1).
 *   - Broadcom's .hcd patch file: vendor commands back to back (opcode le16, length u8, bytes): Write_RAM (0xFC4C, a
 *     le32 address then data) records and Launch_RAM (0xFC4E, address) last; sent after Download_Minidriver (0xFC2E)
 *     (Linux btbcm.c btbcm_patchram, GPL-2.0, read for the format).
 *   - The Exynos UART's divisor (Samsung serial driver, GPL-2.0): UBRDIV = div / 16 - 1, UFRACVAL = div % 16,
 *     div = clock / baud rounded to the nearest.
 *   - Scan results: Inquiry Result (0x02), with RSSI (0x22), Extended (0x2F, EIR), LE Advertising Report (0x3E/0x02);
 *     names from EIR / AD (0x09 complete, 0x08 shortened local name; Core Supplement part A 1.2).
 *   - Groundwork for pairing: Secure Simple Pairing "Just Works" answers (IO Capability Request -> NoInputNoOutput,
 *     User Confirmation Request -> accept). Not used by the driver yet.
 * Nothing here logs an address. */
#ifndef SBOS_BT_HCI_H
#define SBOS_BT_HCI_H
#include <stdint.h>
#include <stddef.h>

#define H4_CMD 0x01
#define H4_ACL 0x02
#define H4_SCO 0x03
#define H4_EVT 0x04

#define BTHCI_SET_EVENT_MASK      0x0C01
#define BTHCI_RESET               0x0C03
#define BTHCI_WRITE_INQUIRY_MODE  0x0C45
#define BTHCI_READ_LOCAL_VERSION  0x1001
#define BTHCI_READ_BD_ADDR        0x1009
#define BTHCI_INQUIRY             0x0401
#define BTHCI_INQUIRY_CANCEL      0x0402
#define BTHCI_IO_CAP_REPLY        0x042B
#define BTHCI_USER_CONFIRM_REPLY  0x042C
#define BTHCI_LE_SET_RANDOM_ADDR  0x2005
#define BTHCI_LE_SET_SCAN_PARAMS  0x200B
#define BTHCI_LE_SET_SCAN_ENABLE  0x200C
#define BCM_UPDATE_BAUD           0xFC18    /* two zero bytes, the rate le32 (Linux hci_bcm.c) */
#define BCM_DOWNLOAD_MINIDRIVER   0xFC2E
#define BCM_WRITE_RAM             0xFC4C
#define BCM_LAUNCH_RAM            0xFC4E
#define BCM_MANUFACTURER          0x000F

#define BTEV_INQUIRY_COMPLETE     0x01
#define BTEV_INQUIRY_RESULT       0x02
#define BTEV_CMD_COMPLETE         0x0E
#define BTEV_CMD_STATUS           0x0F
#define BTEV_HW_ERROR             0x10
#define BTEV_LINK_KEY_NOTIFY      0x18
#define BTEV_INQUIRY_RESULT_RSSI  0x22
#define BTEV_EXT_INQUIRY_RESULT   0x2F
#define BTEV_IO_CAP_REQUEST       0x31
#define BTEV_USER_CONFIRM_REQUEST 0x33
#define BTEV_SSP_COMPLETE         0x36
#define BTEV_LE_META              0x3E
#define BTEV_LE_ADV_REPORT        0x02

/* ---- a log line (no libc) ---- */
struct btl {
    char b[320];
    unsigned n;
};
void btl_init(struct btl *l);
void btl_s(struct btl *l, const char *s);
void btl_u(struct btl *l, uint64_t v);
void btl_i(struct btl *l, int64_t v);
void btl_x(struct btl *l, uint64_t v, unsigned digits);      /* digits hex digits, no 0x */
int bthci_streq(const char *a, const char *b);
void bthci_copy(void *dst, const void *src, unsigned n);

/* ---- H4 out ---- */
/* the command as H4 bytes: 4 + n, or 0 if cap is too small */
unsigned bthci_cmd(uint8_t *out, unsigned cap, uint16_t op, const uint8_t *p, uint8_t n);

/* ---- H4 in: a byte at a time ---- */
struct bthci_rx {
    uint8_t type;                           /* 0: waiting for a type byte */
    uint16_t have, need;
    uint8_t ev[2 + 255];                    /* an event: code, length, parameters */
    uint32_t junk;                          /* bytes that could not start a packet */
    uint32_t skipped;                       /* ACL / SCO packets (none expected) */
};
enum { BTHCI_RX_MORE = 0, BTHCI_RX_EVENT, BTHCI_RX_SKIPPED, BTHCI_RX_JUNK };
void bthci_rx_reset(struct bthci_rx *r);
int bthci_rx_byte(struct bthci_rx *r, uint8_t b);

/* ---- events ---- */
/* Command Complete: 1 with the opcode, the status (the first return byte; 0xFF if none) and what follows it */
int bthci_cmd_complete(const uint8_t *ev, uint16_t *op, uint8_t *status, const uint8_t **ret, uint8_t *retlen);
int bthci_cmd_status(const uint8_t *ev, uint16_t *op, uint8_t *status);
struct bthci_version {
    uint8_t hci_ver, lmp_ver;
    uint16_t hci_rev, manufacturer, lmp_subver;
};
int bthci_version(const uint8_t *ret, uint8_t n, struct bthci_version *v);

/* ---- the .hcd patch ---- */
struct bthci_rec {
    uint16_t op;
    uint8_t n;
    const uint8_t *p;
};
/* the record at *off: 1, 0 at the end, -1 if the file ends inside a record */
int bthci_hcd_next(const uint8_t *d, uint32_t size, uint32_t *off, struct bthci_rec *r);
struct bthci_hcd {
    uint32_t records, writes, bytes, launch;
    char name[48];                          /* the patch's own description ("BCM4361B2 ..."), if it has one */
};
#define BTHCI_HCD_OK 0
#define BTHCI_HCD_EMPTY -1
#define BTHCI_HCD_TRUNC -2
#define BTHCI_HCD_NOTVENDOR -3
#define BTHCI_HCD_SHORTWRITE -4
#define BTHCI_HCD_NOLAUNCH -5
#define BTHCI_HCD_EARLYLAUNCH -6
#define BTHCI_HCD_BADLAUNCH -7
/* the whole file read through before a byte goes to the chip */
int bthci_hcd_check(const uint8_t *d, uint32_t size, struct bthci_hcd *i);
const char *bthci_hcd_why(int e);

/* ---- the Exynos UART's divisor ---- */
struct bthci_div {
    uint32_t ubrdiv, frac, actual;
};
int bthci_uart_div(uint32_t clk_hz, uint32_t baud, struct bthci_div *d);    /* 0 if the clock is too slow */
/* the fastest rate of 3000000, 2000000, 1500000, 1000000, 921600, 460800 (<= top) the clock gives within 1.5 %; 0 none */
uint32_t bthci_pick_baud(uint32_t clk_hz, uint32_t top);

/* ---- scan results ---- */
#define BT_MAX_DEV 32
struct bt_dev {
    uint8_t addr[6];                        /* as HCI gives it (little-endian); for the UI on the phone, never logged */
    uint8_t le, addr_type, has_rssi;
    int8_t rssi;                            /* the strongest heard, dBm */
    uint32_t cod;                           /* class of device (BR/EDR) */
    uint16_t hits;
    char name[33];                          /* printable ASCII and UTF-8 bytes; "" if none heard */
};
struct bthci_devs {
    struct bt_dev d[BT_MAX_DEV];
    unsigned n, reports, full;              /* full: reports of new devices dropped (list full) */
    int inquiry_done;
    uint8_t inquiry_status;
};
void bthci_devs_clear(struct bthci_devs *l);
/* 1 if ev was a scan event (taken into the list) */
int bthci_scan_event(struct bthci_devs *l, const uint8_t *ev);
/* the local name in EIR / AD data (complete before shortened) */
void bthci_ad_name(const uint8_t *d, unsigned n, char *out, unsigned cap);
/* up to max indices into l->d, strongest RSSI first (devices without one last) */
unsigned bthci_devs_sorted(const struct bthci_devs *l, unsigned *idx, unsigned max);

/* ---- pairing groundwork: Secure Simple Pairing "Just Works" ---- */
/* The H4 command that answers an SSP event: IO Capability Request -> IO Capability Request Reply (NoInputNoOutput,
 * no OOB, MITM not required, dedicated bonding); User Confirmation Request -> Reply (accept). 0 if ev is not one. */
unsigned bthci_ssp_answer(const uint8_t *ev, uint8_t *out, unsigned cap);
#endif
