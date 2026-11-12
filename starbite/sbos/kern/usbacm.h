/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's USB device identity (usbacm.c): one CDC-ACM function, the read-only console. usbacm.c implements
 * the descriptor and control-request half of starbite/utility/usb_desc.h (the API the shared DWC3 driver
 * starbite/utility/usb_dwc3.c calls), with no mass storage and no HID: the host sees a serial port and nothing else. */
#ifndef SBOS_USBACM_H
#define SBOS_USBACM_H
#include <stdint.h>

#define USBACM_VID 0x1209U          /* pid.codes (open-source hardware) */
#ifndef USBACM_PID
#define USBACM_PID 0x0001U          /* the project's test PID, as the Utility OS (a Mac that allowed it once allows it) */
#endif
#define USBACM_BCD 0x0300U          /* bcdDevice 3.00: Starbite OS (the Utility OS is 1.00) */
#define USBACM_SERIAL "SBOS"        /* fixed: never the phone's own serial number */

#define USBACM_IF_CTRL 0U
#define USBACM_IF_DATA 1U
#define USBACM_IF_COUNT 2U
#define USBACM_EP_NOTIFY 0x81U      /* interrupt IN, never sent */
#define USBACM_EP_IN 0x82U          /* bulk IN: the console's output */
#define USBACM_EP_OUT 0x02U         /* bulk OUT: what the user types */
#define USBACM_CONFIG_LEN 75U

/* What the host did, as the control requests saw it (the probe and `usb` read it). */
struct usbacm_seen {
    uint32_t setups, get_desc, stalls;
    uint32_t set_address, address;  /* SET_ADDRESS count, the last address */
    uint32_t set_config, config;    /* SET_CONFIGURATION count, the last value */
    uint32_t line_sets, line_state; /* SET_CONTROL_LINE_STATE count, the last value (bit 0 DTR) */
};
extern struct usbacm_seen usbacm_seen;
void usbacm_seen_reset(void);
#endif
