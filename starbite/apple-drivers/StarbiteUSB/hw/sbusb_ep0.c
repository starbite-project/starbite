/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB control requests (sbusb_ep0.h).
 */
#include "sbusb_ep0.h"
#include "sbusb_ncm.h"
#include "sbusb_io.h"

enum { REQ_GET_STATUS = 0, REQ_CLEAR_FEATURE = 1, REQ_SET_FEATURE = 3, REQ_SET_ADDRESS = 5, REQ_GET_DESCRIPTOR = 6,
       REQ_GET_CONFIGURATION = 8, REQ_SET_CONFIGURATION = 9, REQ_GET_INTERFACE = 10, REQ_SET_INTERFACE = 11 };

void
sbusb_state_reset(struct sbusb_state *s)
{
	s->config = 0;
	s->data_alt = 0;
	s->packet_filter = 0;
	s->ntb_in_max = SBUSB_NTB_MAX;
}

void
sbusb_state_init(struct sbusb_state *s, const char *serial)
{
	sbusb_ident_init(&s->id, serial);
	s->full_speed = 0;
	sbusb_state_reset(s);
}

static int
known_endpoint(unsigned addr)
{
	return addr == 0x00u || addr == 0x80u || addr == SBUSB_EP_NOTIFY || addr == SBUSB_EP_IN || addr == SBUSB_EP_OUT;
}

static void
standard(struct sbusb_state *s, unsigned type, unsigned req, unsigned value, unsigned index, struct sbusb_ep0_res *r)
{
	unsigned recipient = type & 0x1fu, in = type & 0x80u;

	switch (req) {
	case REQ_GET_STATUS:
		if (!in || (recipient == 1 && index > SBUSB_IF_DATA) || (recipient == 2 && !known_endpoint(index & 0xffu)) ||
		    recipient > 2) {
			return;
		}
		r->data[0] = recipient == 0 ? 1 : 0;    /* device: self-powered; halt is never left set */
		r->data[1] = 0;
		r->len = 2;
		r->action = SBUSB_EP0_IN;
		return;
	case REQ_CLEAR_FEATURE:
	case REQ_SET_FEATURE:
		if (in) {
			return;
		}
		if (recipient == 2 && value == 0 && known_endpoint(index & 0xffu)) {    /* ENDPOINT_HALT */
			if ((index & 0x0fu) != 0) {
				r->halt_ep = (int)(index & 0xffu);
				r->halt_set = req == REQ_SET_FEATURE;
			}
			r->action = SBUSB_EP0_ACK;
		} else if (recipient == 0 && value == 1 && req == REQ_CLEAR_FEATURE) {  /* remote wake-up: never on */
			r->action = SBUSB_EP0_ACK;
		}
		return;
	case REQ_SET_ADDRESS:
		if (type == 0x00u && value < 128) {
			r->set_address = (int)value;
			r->action = SBUSB_EP0_ACK;
		}
		return;
	case REQ_GET_DESCRIPTOR:
		if (type != 0x80u) {
			return;
		}
		switch (value >> 8) {
		case 1:
			r->len = sbusb_desc_device(&s->id, r->data);
			break;
		case 2:
			r->len = (value & 0xffu) == 0 ? sbusb_desc_config(r->data, s->full_speed, 2) : 0;
			break;
		case 3:
			r->len = sbusb_desc_string(&s->id, value & 0xffu, r->data);
			break;
		case 6:
			r->len = sbusb_desc_qualifier(r->data);
			break;
		case 7:
			r->len = (value & 0xffu) == 0 ? sbusb_desc_config(r->data, !s->full_speed, 7) : 0;
			break;
		default:
			r->len = 0;             /* BOS and the rest: not a USB 2.1 device */
		}
		if (r->len != 0) {
			r->action = SBUSB_EP0_IN;
		}
		return;
	case REQ_GET_CONFIGURATION:
		if (type == 0x80u) {
			r->data[0] = s->config;
			r->len = 1;
			r->action = SBUSB_EP0_IN;
		}
		return;
	case REQ_SET_CONFIGURATION:
		if (type == 0x00u && value <= 1) {
			s->config = (uint8_t)value;
			s->data_alt = 0;
			r->set_config = (int)value;
			r->action = SBUSB_EP0_ACK;
		}
		return;
	case REQ_GET_INTERFACE:
		if (type == 0x81u && s->config != 0 && index <= SBUSB_IF_DATA) {
			r->data[0] = index == SBUSB_IF_DATA ? s->data_alt : 0;
			r->len = 1;
			r->action = SBUSB_EP0_IN;
		}
		return;
	case REQ_SET_INTERFACE:
		if (type != 0x01u || s->config == 0) {
			return;
		}
		if (index == SBUSB_IF_CTRL && value == 0) {
			r->action = SBUSB_EP0_ACK;
		} else if (index == SBUSB_IF_DATA && value <= 1) {
			s->data_alt = (uint8_t)value;
			r->set_alt = (int)value;
			r->action = SBUSB_EP0_ACK;
		}
		return;
	default:
		return;
	}
}

static void
ncm_class(struct sbusb_state *s, unsigned type, unsigned req, unsigned value, unsigned index, unsigned length,
    struct sbusb_ep0_res *r)
{
	if (index != SBUSB_IF_CTRL) {
		return;
	}
	if (type == 0xa1u) {
		switch (req) {
		case SBUSB_REQ_GET_NTB_PARAMETERS:
			r->len = sbusb_ncm_params(r->data);
			break;
		case SBUSB_REQ_GET_NTB_INPUT_SIZE:
			sbusb_put32(r->data, s->ntb_in_max);
			r->len = 4;
			break;
		case SBUSB_REQ_GET_NTB_FORMAT:
			sbusb_put16(r->data, 0);                /* 16-bit */
			r->len = 2;
			break;
		default:
			return;
		}
		r->action = SBUSB_EP0_IN;
	} else if (type == 0x21u) {
		switch (req) {
		case SBUSB_REQ_SET_ETH_PACKET_FILTER:
			if (length == 0) {
				s->packet_filter = (uint16_t)value;
				r->action = SBUSB_EP0_ACK;
			}
			break;
		case SBUSB_REQ_SET_NTB_FORMAT:
			if (length == 0 && value == 0) {
				r->action = SBUSB_EP0_ACK;
			}
			break;
		case SBUSB_REQ_SET_NTB_INPUT_SIZE:
			if (length == 4) {
				r->len = 4;
				r->action = SBUSB_EP0_OUT;
			}
			break;
		default:
			break;
		}
	}
}

void
sbusb_ep0_setup(struct sbusb_state *s, const uint8_t setup[8], struct sbusb_ep0_res *r)
{
	unsigned type = setup[0], req = setup[1];
	unsigned value = sbusb_get16(setup + 2), index = sbusb_get16(setup + 4), length = sbusb_get16(setup + 6);

	r->action = SBUSB_EP0_STALL;
	r->len = 0;
	r->set_address = r->set_config = r->set_alt = r->halt_ep = -1;
	r->halt_set = 0;
	if ((type & 0x60u) == 0x00u) {
		standard(s, type, req, value, index, r);
	} else if ((type & 0x60u) == 0x20u && (type & 0x1fu) == 1) {
		ncm_class(s, type, req, value, index, length, r);
	}
	if (r->action == SBUSB_EP0_IN) {
		if (r->len > length) {
			r->len = length;
		}
		if (r->len == 0) {
			r->action = SBUSB_EP0_STALL;    /* an IN request for nothing */
		}
	}
}

int
sbusb_ep0_out(struct sbusb_state *s, const uint8_t setup[8], const uint8_t *data, unsigned len)
{
	if (setup[0] == 0x21u && setup[1] == SBUSB_REQ_SET_NTB_INPUT_SIZE && len == 4) {
		uint32_t v = sbusb_get32(data);
		if (v < SBUSB_NTB_MIN || v > SBUSB_NTB_MAX) {
			return 0;
		}
		s->ntb_in_max = v;
		return 1;
	}
	return 0;
}
