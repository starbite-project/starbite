/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: what the control endpoint answers. Pure: a SETUP packet and the device state in, the action
 * out. The controller side (stages, TRBs) is sbusb_dwc3.c. USB 2.0 chapter 9; NCM 1.0 6.2; ECM 1.2 6.2.4.
 */
#ifndef SBUSB_EP0_H
#define SBUSB_EP0_H
#include <stdint.h>
#include "sbusb_desc.h"

#define SBUSB_EP0_STALL 0
#define SBUSB_EP0_ACK   1       /* no data stage */
#define SBUSB_EP0_IN    2       /* data[0..len) to the host */
#define SBUSB_EP0_OUT   3       /* len bytes from the host, then sbusb_ep0_out() */

struct sbusb_state {
	struct sbusb_ident id;
	int full_speed;
	uint8_t config;                 /* 0 or 1 */
	uint8_t data_alt;               /* alternate setting of the data interface: 0 or 1 */
	uint16_t packet_filter;         /* the last SetEthernetPacketFilter value */
	uint32_t ntb_in_max;            /* the host's NTB input size: SBUSB_NTB_MIN..SBUSB_NTB_MAX */
};

struct sbusb_ep0_res {
	int action;
	unsigned len;
	uint8_t data[256];
	int set_address;                /* -1, or the address */
	int set_config;                 /* -1, or 0 / 1 */
	int set_alt;                    /* -1, or the data interface's new alternate setting */
	int halt_ep;                    /* -1, or a USB endpoint address ... */
	int halt_set;                   /* ... whose halt is set (1) or cleared (0) */
};

void sbusb_state_init(struct sbusb_state *s, const char *serial);
/* a bus reset: address 0, not configured, the class defaults back */
void sbusb_state_reset(struct sbusb_state *s);
void sbusb_ep0_setup(struct sbusb_state *s, const uint8_t setup[8], struct sbusb_ep0_res *r);
/* The data stage of a request sbusb_ep0_setup answered with SBUSB_EP0_OUT. 1 accepted, 0 refused (stall). */
int sbusb_ep0_out(struct sbusb_state *s, const uint8_t setup[8], const uint8_t *data, unsigned len);

#endif
