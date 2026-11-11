/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: the USB identity and descriptors of the phase 1 device: one CDC-NCM function.
 *   interface 0  CDC control, subclass 0x0d (NCM)     EP 0x81 interrupt IN (notifications)
 *   interface 1  CDC data, protocol 0x01 (NCM data)   alt 0: no endpoints; alt 1: EP 0x82 bulk IN, EP 0x02 bulk OUT
 * USB 2.0 chapter 9; CDC 1.2 (header, union); ECM 1.2 section 5.4 (Ethernet networking functional
 * descriptor); NCM 1.0 section 5.2 (NCM functional descriptor).
 */
#ifndef SBUSB_DESC_H
#define SBUSB_DESC_H
#include <stdint.h>

#define SBUSB_VID               0x18d1u         /* the loader's (shim/starlte/usb_desc.h USB_VID) */
#define SBUSB_PID               0x4ee0u         /* the loader's (USB_PID): an identity the Mac has already approved */
#define SBUSB_BCD_DEVICE        0x0200u         /* the loader is 0x0100: tells the two apart on the Mac */

#define SBUSB_IF_CTRL           0u
#define SBUSB_IF_DATA           1u
#define SBUSB_EP_NOTIFY         0x81u
#define SBUSB_EP_IN             0x82u
#define SBUSB_EP_OUT            0x02u
#define SBUSB_EP0_MPS           64u
#define SBUSB_NOTIFY_MPS        16u
#define SBUSB_CONFIG_LEN        94u
#define SBUSB_MAX_SEGMENT       1514u           /* Ethernet frame without FCS */

/* DWC3 physical endpoint of a USB endpoint address */
#define SBUSB_PHYS(addr)        ((((unsigned)(addr) & 0x0fu) << 1) | (((unsigned)(addr) & 0x80u) ? 1u : 0u))

#define SBUSB_STR_LANG          0u
#define SBUSB_STR_MANUFACTURER  1u
#define SBUSB_STR_PRODUCT       2u
#define SBUSB_STR_SERIAL        3u
#define SBUSB_STR_MAC           4u
#define SBUSB_STR_FUNCTION      5u

struct sbusb_ident {
	uint16_t vid, pid, bcd;
	char serial[33];
	uint8_t dev_mac[6];     /* the phone's end (IOEthernetController address) */
	uint8_t host_mac[6];    /* the Mac's end (iMACAddress) */
};

/* Defaults; MACs are locally administered and derived from the serial, so the Mac sees the same adapter each boot. */
void sbusb_ident_init(struct sbusb_ident *id, const char *serial);

unsigned sbusb_bulk_mps(int full_speed);                        /* 64 or 512 */
unsigned sbusb_desc_device(const struct sbusb_ident *id, uint8_t *out);         /* 18 bytes */
unsigned sbusb_desc_qualifier(uint8_t *out);                                    /* 10 bytes */
/* configuration (type 2) or other-speed configuration (type 7) for a speed: SBUSB_CONFIG_LEN bytes */
unsigned sbusb_desc_config(uint8_t *out, int full_speed, unsigned type);
/* string index i; 0 when there is none. out: at least 2 + 2 x 63 bytes */
unsigned sbusb_desc_string(const struct sbusb_ident *id, unsigned i, uint8_t *out);

#endif
