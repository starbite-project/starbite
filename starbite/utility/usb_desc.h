/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's USB device: descriptors and the chapter 9 / CDC-ACM control requests (usb_desc.c).
 * Pure code (tools/usb-fastboot/host_test.c checks it on the Mac).
 *
 * One configuration, three interfaces (docs/USB-FASTBOOT.md section 2):
 *   0  CDC-ACM control  (IAD 0-1)   EP 0x81 interrupt IN (notifications, never sent)
 *   1  CDC-ACM data                 EP 0x82 bulk IN, EP 0x02 bulk OUT   -> /dev/cu.usbmodem*
 *   2  fastboot 0xff/0x42/0x03      EP 0x83 bulk IN, EP 0x03 bulk OUT   -> `fastboot devices`
 * VID:PID 18d1:4ee0 (Google's fastboot ID; the fastboot tool matches the interface class, not
 * the ID).
 */
#pragma once
#include <stdint.h>

#define USB_VID 0x1209U            /* pid.codes (open-source hardware) */
#define USB_PID 0x0001U            /* pid.codes test PID: the Starbite Utility OS (not the loader's 18d1:4ee0) */
#define USB_BCD_DEVICE 0x0100U

#define USB_IF_ACM_CTRL 0U
#define USB_IF_ACM_DATA 1U
#define USB_IF_FASTBOOT 2U         /* Utility OS: USB mass storage (bulk-only) on the same interface/endpoints */
#define USB_IF_MSC USB_IF_FASTBOOT
#define USB_IF_HID      3U         /* Utility OS: keyboard, mouse, media keys (report IDs 1, 2, 3) */
#define USB_IF_COUNT    4U
#define USB_EP_ACM_NOTIFY 0x81U
#define USB_EP_ACM_IN     0x82U
#define USB_EP_ACM_OUT    0x02U
#define USB_EP_FB_IN      0x83U
#define USB_EP_FB_OUT     0x03U
#define USB_EP_MSC_IN  USB_EP_FB_IN
#define USB_EP_MSC_OUT USB_EP_FB_OUT
#define USB_EP_HID_IN  0x84U
#define USB_HID_MPS    16U
/* Mass storage class requests, answered from msc.c's state */
extern volatile unsigned usb_msc_max_lun;   /* GET_MAX_LUN answer */
extern volatile unsigned usb_msc_reset;     /* set by Bulk-Only Mass Storage Reset */

/* DWC3 physical endpoint of a USB endpoint address: 2 x number + 1 for IN */
#define USB_PHYS(addr) ((((unsigned)(addr) & 0x0fU) << 1) | (((unsigned)(addr) & 0x80U) ? 1U : 0U))

#define USB_EP0_MPS 64U
#define USB_CONFIG_LEN 123U        /* + HID: interface 9, HID descriptor 9, endpoint 7 */

/* what the control endpoint does with a setup packet */
#define CH9_STALL 0
#define CH9_ACK   1   /* no data stage: status stage only */
#define CH9_IN    2   /* data stage to the host: data[0..len) */
#define CH9_OUT   3   /* data stage from the host: len bytes, then usb_ch9_out() */

struct ch9_res {
    int action;
    unsigned len;
    uint8_t data[256];
    int set_address;   /* -1, or the address (DCFG takes it at once: DWC3 applies it after status) */
    int set_config;    /* -1, or the configuration value (0 or 1) */
    int clear_halt;    /* -1, or the USB endpoint address whose halt was cleared */
    int line_changed;  /* SET_CONTROL_LINE_STATE arrived */
};

struct usb_dev_state {
    int full_speed;            /* 1: bulk max packet 64 (a full-speed port), else 512 */
    uint8_t config;
    uint8_t line_coding[7];    /* dwDTERate, bCharFormat, bParityType, bDataBits */
    uint16_t line_state;       /* bit 0 DTR, bit 1 RTS */
    char serial[33];
};

void usb_ch9_init(struct usb_dev_state *s, const char *serial);
void usb_ch9_setup(struct usb_dev_state *s, const uint8_t setup[8], struct ch9_res *r);
/* The data stage of an OUT request usb_ch9_setup accepted (CH9_OUT). */
void usb_ch9_out(struct usb_dev_state *s, const uint8_t setup[8], const uint8_t *data, unsigned len);

unsigned usb_desc_device(uint8_t *out);
/* The configuration (type 2) or other-speed configuration (type 7) for a speed. */
unsigned usb_desc_config(uint8_t *out, int full_speed, uint8_t type);
unsigned usb_desc_qualifier(uint8_t *out);
/* String index i (0: language IDs); 0 if there is none. out holds 2 + 2 x 63 bytes. */
unsigned usb_desc_string(uint8_t *out, unsigned i, const char *serial);
/* max packet size of a USB endpoint address at this speed */
unsigned usb_ep_mps(unsigned addr, int full_speed);
/* interrupt endpoints: the bInterval the descriptor gives (0: a bulk endpoint) */
unsigned usb_ep_interval(unsigned addr, int full_speed);
/* the endpoints the configuration uses, for the DWC3 driver's SET_CONFIGURATION */
unsigned usb_desc_eps(const uint8_t **list);
unsigned usb_hid_report_desc(uint8_t *out);
