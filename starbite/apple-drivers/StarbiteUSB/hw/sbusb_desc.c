/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB descriptors (sbusb_desc.h).
 */
#include "sbusb_desc.h"
#include "sbusb_io.h"

enum { DT_DEVICE = 1, DT_CONFIG = 2, DT_STRING = 3, DT_INTERFACE = 4, DT_ENDPOINT = 5, DT_QUALIFIER = 6,
       DT_OTHER_SPEED = 7, DT_IAD = 11, DT_CS_INTERFACE = 0x24 };

void
sbusb_ident_init(struct sbusb_ident *id, const char *serial)
{
	uint32_t h = 2166136261u;       /* FNV-1a over the serial */
	unsigned n = 0;

	for (n = 0; n < sizeof(*id); ++n) {
		((uint8_t *)id)[n] = 0;
	}
	n = 0;
	id->vid = SBUSB_VID;
	id->pid = SBUSB_PID;
	id->bcd = SBUSB_BCD_DEVICE;
	if (serial == 0 || serial[0] == 0) {
		serial = "STARBITE";
	}
	for (; serial[n] != 0 && n + 1 < sizeof(id->serial); ++n) {
		char c = serial[n];
		id->serial[n] = (c > 0x20 && c < 0x7f) ? c : '_';
		h = (h ^ (uint8_t)c) * 16777619u;
	}
	id->serial[n] = 0;
	id->dev_mac[0] = 0x02;          /* locally administered, unicast */
	id->dev_mac[1] = 0x53;          /* 'S' */
	id->dev_mac[2] = 0x42;          /* 'B' */
	id->dev_mac[3] = (uint8_t)(h >> 16);
	id->dev_mac[4] = (uint8_t)(h >> 8);
	id->dev_mac[5] = (uint8_t)(h & 0xfeu);
	for (n = 0; n < 6; ++n) {
		id->host_mac[n] = id->dev_mac[n];
	}
	id->host_mac[5] |= 1u;
}

unsigned
sbusb_bulk_mps(int full_speed)
{
	return full_speed ? 64u : 512u;
}

unsigned
sbusb_desc_device(const struct sbusb_ident *id, uint8_t *o)
{
	o[0] = 18; o[1] = DT_DEVICE;
	sbusb_put16(o + 2, 0x0200);
	o[4] = 0xef; o[5] = 0x02; o[6] = 0x01;          /* miscellaneous / common / IAD */
	o[7] = SBUSB_EP0_MPS;
	sbusb_put16(o + 8, id->vid);
	sbusb_put16(o + 10, id->pid);
	sbusb_put16(o + 12, id->bcd);
	o[14] = SBUSB_STR_MANUFACTURER; o[15] = SBUSB_STR_PRODUCT; o[16] = SBUSB_STR_SERIAL;
	o[17] = 1;
	return 18;
}

unsigned
sbusb_desc_qualifier(uint8_t *o)
{
	o[0] = 10; o[1] = DT_QUALIFIER;
	sbusb_put16(o + 2, 0x0200);
	o[4] = 0xef; o[5] = 0x02; o[6] = 0x01;
	o[7] = SBUSB_EP0_MPS; o[8] = 1; o[9] = 0;
	return 10;
}

static unsigned
put(uint8_t *o, unsigned n, const uint8_t *src, unsigned len)
{
	for (unsigned i = 0; i < len; ++i) {
		o[n + i] = src[i];
	}
	return n + len;
}

static unsigned
endpoint(uint8_t *o, unsigned n, unsigned addr, unsigned attr, unsigned mps, unsigned interval)
{
	o[n] = 7; o[n + 1] = DT_ENDPOINT; o[n + 2] = (uint8_t)addr; o[n + 3] = (uint8_t)attr;
	sbusb_put16(o + n + 4, mps);
	o[n + 6] = (uint8_t)interval;
	return n + 7;
}

unsigned
sbusb_desc_config(uint8_t *o, int fs, unsigned type)
{
	static const uint8_t iad[8] = { 8, DT_IAD, SBUSB_IF_CTRL, 2, 0x02, 0x0d, 0x00, SBUSB_STR_FUNCTION };
	static const uint8_t if_ctrl[9] = { 9, DT_INTERFACE, SBUSB_IF_CTRL, 0, 1, 0x02, 0x0d, 0x00, SBUSB_STR_FUNCTION };
	static const uint8_t cdc_header[5] = { 5, DT_CS_INTERFACE, 0x00, 0x10, 0x01 };          /* bcdCDC 1.10 */
	static const uint8_t cdc_union[5] = { 5, DT_CS_INTERFACE, 0x06, SBUSB_IF_CTRL, SBUSB_IF_DATA };
	static const uint8_t cdc_ether[13] = { 13, DT_CS_INTERFACE, 0x0f, SBUSB_STR_MAC,
		                               0, 0, 0, 0,                                      /* bmEthernetStatistics */
		                               SBUSB_MAX_SEGMENT & 0xff, SBUSB_MAX_SEGMENT >> 8,
		                               0, 0,                                            /* wNumberMCFilters */
		                               0 };                                             /* bNumberPowerFilters */
	/* bmNetworkCapabilities bit 0: SetEthernetPacketFilter is answered. Nothing else is offered. */
	static const uint8_t cdc_ncm[6] = { 6, DT_CS_INTERFACE, 0x1a, 0x00, 0x01, 0x01 };       /* bcdNcmVersion 1.00 */
	static const uint8_t if_data0[9] = { 9, DT_INTERFACE, SBUSB_IF_DATA, 0, 0, 0x0a, 0x00, 0x01, 0 };
	static const uint8_t if_data1[9] = { 9, DT_INTERFACE, SBUSB_IF_DATA, 1, 2, 0x0a, 0x00, 0x01, 0 };
	unsigned n = 9, mps = sbusb_bulk_mps(fs);

	o[0] = 9; o[1] = (uint8_t)type;
	sbusb_put16(o + 2, SBUSB_CONFIG_LEN);
	o[4] = 2;               /* interfaces */
	o[5] = 1;               /* configuration value */
	o[6] = 0;
	o[7] = 0xc0;            /* self-powered (the battery) */
	o[8] = 50;              /* 100 mA */
	n = put(o, n, iad, sizeof(iad));
	n = put(o, n, if_ctrl, sizeof(if_ctrl));
	n = put(o, n, cdc_header, sizeof(cdc_header));
	n = put(o, n, cdc_union, sizeof(cdc_union));
	n = put(o, n, cdc_ether, sizeof(cdc_ether));
	n = put(o, n, cdc_ncm, sizeof(cdc_ncm));
	n = endpoint(o, n, SBUSB_EP_NOTIFY, 0x03, SBUSB_NOTIFY_MPS, fs ? 32 : 9);      /* 32 ms either way */
	n = put(o, n, if_data0, sizeof(if_data0));
	n = put(o, n, if_data1, sizeof(if_data1));
	n = endpoint(o, n, SBUSB_EP_IN, 0x02, mps, 0);
	n = endpoint(o, n, SBUSB_EP_OUT, 0x02, mps, 0);
	return n;
}

static unsigned
ascii_string(uint8_t *o, const char *s)
{
	unsigned n = 0;
	for (; s[n] != 0 && n < 63; ++n) {
		o[2 + 2 * n] = (uint8_t)s[n];
		o[3 + 2 * n] = 0;
	}
	o[0] = (uint8_t)(2 + 2 * n);
	o[1] = DT_STRING;
	return o[0];
}

unsigned
sbusb_desc_string(const struct sbusb_ident *id, unsigned i, uint8_t *o)
{
	static const char hex[] = "0123456789ABCDEF";
	char mac[13];

	switch (i) {
	case SBUSB_STR_LANG:
		o[0] = 4; o[1] = DT_STRING; o[2] = 0x09; o[3] = 0x04;      /* English (US) */
		return 4;
	case SBUSB_STR_MANUFACTURER:
		return ascii_string(o, "Starbite");
	case SBUSB_STR_PRODUCT:
		return ascii_string(o, "Starbite S9 network (NCM)");
	case SBUSB_STR_SERIAL:
		return ascii_string(o, id->serial);
	case SBUSB_STR_MAC:             /* ECM 1.2 5.4: 12 upper-case hexadecimal digits, no separators */
		for (unsigned b = 0; b < 6; ++b) {
			mac[2 * b] = hex[id->host_mac[b] >> 4];
			mac[2 * b + 1] = hex[id->host_mac[b] & 0xfu];
		}
		mac[12] = 0;
		return ascii_string(o, mac);
	case SBUSB_STR_FUNCTION:
		return ascii_string(o, "CDC NCM");
	default:
		return 0;
	}
}
