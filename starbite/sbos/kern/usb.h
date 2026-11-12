/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: USB device mode (usb.c; docs/drivers/USB.md). The phone shows up on a computer as one serial port
 * (CDC-ACM: /dev/cu.usbmodem* on a Mac), a read-only console that streams the kernel log and answers `help`, `ver`,
 * `bat`, `log`, `follow`, `usb`. Nothing else is offered: no drives, no partitions, no command that writes storage or
 * changes a setting. The controller code is the Utility OS's polling DWC3 driver (starbite/utility/usb_dwc3.c,
 * usb_phy.c, linked as they are); usb.c is the glue: the cable, the D+/D- switch, the link's life, the console. */
#ifndef SBOS_USB_H
#define SBOS_USB_H
#include <stdint.h>

/* 1: the kernel starts watching the cable at boot (usb_init). 0 (the default until the drvprobe's level 1 has passed
 * on the phone): off until usb_enable(1). The self-test images never start it (the probe drives it there). */
#ifndef SBOS_USB_AUTO
#define SBOS_USB_AUTO 0
#endif

/* ---- the kernel's calls ---------------------------------------------------------------------------------------- */
void usb_init(void);                 /* main.c, after the services: arms KT_USB when SBOS_USB_AUTO */
void usb_timer(unsigned id);         /* main.c dispatch_timer: KT_USB */
void usb_enable(int on);             /* on: watch the cable; off: the link down, the PHY as found, the timer gone */

enum {
    USB_OFF = 0,        /* switched off, or no cable */
    USB_CHARGER,        /* a wall charger (BC1.2 DCP): nothing to talk to */
    USB_WAIT,           /* a cable, its type not known yet (up to 2 s) */
    USB_UP,             /* the controller runs: enumerating, or configured by a host */
    USB_RETRY,          /* the start failed (usb_status().why): tried again after 5, 10, 20, 40, 60 s */
};

struct usb_status {
    int32_t state;                   /* USB_* */
    int32_t enabled;
    int32_t vbus;                    /* the MAX77705's VBUSDET (-1 not read) */
    int32_t chgtyp;                  /* BC1.2: 0 none yet, 1 computer (SDP), 2 charging port (CDP), 3 charger (DCP) */
    int32_t configured;              /* a host set configuration 1 */
    int32_t terminal;                /* a terminal has the port open (DTR) */
    int32_t full_speed;              /* 1: 12 Mbit/s, 0: 480 Mbit/s */
    int32_t address;                 /* the host's address for us, 0 none */
    int32_t ctrl1;                   /* the D+/D- switch (MAX77705 CONTROL1) as read back, -1 not read */
    uint32_t starts, relinks, fails, plugs;
    uint32_t tx_bytes, rx_bytes, tx_dropped;
    const char *why;                 /* the last failure or stop, plain words ("" none) */
};
void usb_status(struct usb_status *s);
/* One line for the UI, plain words: "Not connected", "Charger (no data)", "Connected to a computer", ... */
const char *usb_why(void);

/* ---- the drivers track probe (drvprobe.h) ---------------------------------------------------------------------
 * level 0: reads only: the MAX77705 USB-C block (VBUS, charger type, CC), the USB clock gates, PMU and PHY
 *          registers, and the DWC3's GSNPSID / GCTL / DCTL / DSTS / DCFG / event buffer when its clock gates are
 *          not forced off. Budget 1 s (one call).
 * level 1: with VBUS from a computer: D+/D- to the AP, the PHY and the core up as the CDC-ACM console for at most
 *          10 s, whether the host set an address and configured it, then usbd_stop (the loader's own stop: soft
 *          disconnect, transfers ended, events off, core soft reset) and every PHY / PMU / clock-gate register the
 *          start changed written back as found. Budget 15 s. Without VBUS: PASS "skipped".
 * level 2: nothing (returns 1 at once). */
int drv_usb_probe(int level, unsigned *wait_ms);
void drv_usb_abort(void);
#define DRV_USB_BUDGET_MS 15000U

/* ---- pure parts (kern/host/usb-test.c) ------------------------------------------------------------------------- */
enum { USB_CMD_NONE = 0, USB_CMD_HELP, USB_CMD_VER, USB_CMD_BAT, USB_CMD_LOG, USB_CMD_FOLLOW, USB_CMD_USB,
       USB_CMD_UNKNOWN, USB_CMD_BAD };
/* One console line: the command and its number (log N: N lines, 0 = all; follow on|off: 1 / 0). */
int usb_cmd_parse(const char *line, unsigned *arg);
/* What one VBUS reading asks for: USB_OFF, USB_CHARGER, USB_WAIT or USB_UP. */
int usb_decide(int vbus, int chgtyp, uint64_t ms_since_vbus, int otg_out);
#endif
