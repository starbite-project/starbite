/* SPDX-License-Identifier: APSL-2.0
 * Host test: the descriptor bytes against USB 2.0 chapter 9, CDC 1.2, ECM 1.2 and NCM 1.0.
 */
#include "mock.h"

static void
walk_config(int fs, unsigned type)
{
	uint8_t c[256];
	unsigned n = sbusb_desc_config(c, fs, type), at = 0, ifaces = 0, eps = 0, eps_expected = 0, alt1 = 0;
	unsigned seen_header = 0, seen_union = 0, seen_ether = 0, seen_ncm = 0, in_ctrl = 0, mps = fs ? 64 : 512;

	CHECK_EQ(n, SBUSB_CONFIG_LEN);
	CHECK_EQ(c[0], 9);
	CHECK_EQ(c[1], type);
	CHECK_EQ(sbusb_get16(c + 2), n);                /* wTotalLength */
	CHECK_EQ(c[4], 2);                              /* bNumInterfaces */
	CHECK_EQ(c[5], 1);                              /* bConfigurationValue */
	CHECK(c[7] & 0x80);                             /* bmAttributes bit 7 must be set */
	CHECK(c[7] & 0x40);                             /* self-powered */
	CHECK_EQ(c[7] & 0x1f, 0);
	while (at < n) {
		const uint8_t *d = c + at;
		CHECK(d[0] >= 2 && at + d[0] <= n);
		if (d[0] < 2) {
			break;
		}
		switch (d[1]) {
		case 2: case 7:
			CHECK_EQ(at, 0);
			break;
		case 11:                                /* IAD right after the configuration, before its interfaces */
			CHECK_EQ(d[0], 8);
			CHECK_EQ(at, 9);
			CHECK_EQ(d[2], 0); CHECK_EQ(d[3], 2);
			CHECK_EQ(d[4], 0x02); CHECK_EQ(d[5], 0x0d); CHECK_EQ(d[6], 0x00);
			break;
		case 4:
			CHECK_EQ(d[0], 9);
			CHECK_EQ(eps, eps_expected);    /* the interface before had as many endpoints as it said */
			eps = 0;
			eps_expected = d[4];
			if (d[2] == 0) {
				in_ctrl = 1;
				CHECK_EQ(d[3], 0);
				CHECK_EQ(d[4], 1);
				CHECK_EQ(d[5], 0x02);   /* Communications */
				CHECK_EQ(d[6], 0x0d);   /* Network Control Model */
				CHECK_EQ(d[7], 0x00);
				++ifaces;
			} else {
				in_ctrl = 0;
				CHECK_EQ(d[2], 1);
				CHECK_EQ(d[5], 0x0a);   /* CDC data */
				CHECK_EQ(d[6], 0x00);
				CHECK_EQ(d[7], 0x01);   /* NCM data: network transfer blocks */
				if (d[3] == 0) {
					CHECK_EQ(d[4], 0);      /* NCM 1.0 5.3: alt 0 has no endpoints */
					++ifaces;
				} else {
					CHECK_EQ(d[3], 1);
					CHECK_EQ(d[4], 2);
					alt1 = 1;
				}
			}
			break;
		case 0x24:
			CHECK(in_ctrl);                 /* functional descriptors belong to the control interface */
			if (d[2] == 0x00) {
				CHECK_EQ(d[0], 5);
				CHECK(!seen_union && !seen_ether && !seen_ncm);         /* the header comes first */
				CHECK(sbusb_get16(d + 3) >= 0x0110);
				seen_header = 1;
			} else if (d[2] == 0x06) {
				CHECK_EQ(d[0], 5);
				CHECK_EQ(d[3], 0); CHECK_EQ(d[4], 1);
				seen_union = 1;
			} else if (d[2] == 0x0f) {
				CHECK_EQ(d[0], 13);
				CHECK_EQ(d[3], SBUSB_STR_MAC);
				CHECK_EQ(sbusb_get32(d + 4), 0);
				CHECK_EQ(sbusb_get16(d + 8), 1514);
				CHECK_EQ(sbusb_get16(d + 10), 0);
				CHECK_EQ(d[12], 0);
				seen_ether = 1;
			} else if (d[2] == 0x1a) {
				CHECK_EQ(d[0], 6);
				CHECK_EQ(sbusb_get16(d + 3), 0x0100);
				CHECK_EQ(d[5], 0x01);   /* only SetEthernetPacketFilter */
				seen_ncm = 1;
			} else {
				CHECK(0);
			}
			break;
		case 5:
			CHECK_EQ(d[0], 7);
			++eps;
			if (d[2] == SBUSB_EP_NOTIFY) {
				CHECK(in_ctrl);
				CHECK_EQ(d[3], 3);
				CHECK_EQ(sbusb_get16(d + 4), 16);
				CHECK_EQ(d[6], fs ? 32 : 9);    /* 32 ms: frames, or 2^(9-1) microframes */
			} else {
				CHECK(alt1);
				CHECK(d[2] == SBUSB_EP_IN || d[2] == SBUSB_EP_OUT);
				CHECK_EQ(d[3], 2);
				CHECK_EQ(sbusb_get16(d + 4), mps);
			}
			break;
		default:
			CHECK(0);
		}
		at += d[0];
	}
	CHECK_EQ(at, n);
	CHECK_EQ(eps, eps_expected);
	CHECK_EQ(ifaces, 2);
	CHECK(seen_header && seen_union && seen_ether && seen_ncm && alt1);
}

int
main(void)
{
	struct sbusb_ident id;
	uint8_t b[256];
	unsigned n;

	sbusb_ident_init(&id, "0000000000000001");
	n = sbusb_desc_device(&id, b);
	CHECK_EQ(n, 18); CHECK_EQ(b[0], 18); CHECK_EQ(b[1], 1);
	CHECK_EQ(sbusb_get16(b + 2), 0x0200);
	CHECK_EQ(b[4], 0xef); CHECK_EQ(b[5], 0x02); CHECK_EQ(b[6], 0x01);       /* IAD device (USB ECN) */
	CHECK_EQ(b[7], 64);
	CHECK_EQ(sbusb_get16(b + 8), 0x18d1);           /* the loader's VID:PID (shim/starlte/usb_desc.h) */
	CHECK_EQ(sbusb_get16(b + 10), 0x4ee0);
	CHECK_EQ(sbusb_get16(b + 12), 0x0200);          /* not the loader's bcdDevice 0x0100 */
	CHECK_EQ(b[14], 1); CHECK_EQ(b[15], 2); CHECK_EQ(b[16], 3); CHECK_EQ(b[17], 1);
	n = sbusb_desc_qualifier(b);
	CHECK_EQ(n, 10); CHECK_EQ(b[0], 10); CHECK_EQ(b[1], 6); CHECK_EQ(b[8], 1); CHECK_EQ(b[9], 0);

	walk_config(0, 2);
	walk_config(1, 2);
	walk_config(0, 7);
	walk_config(1, 7);

	/* strings */
	n = sbusb_desc_string(&id, 0, b);
	CHECK_EQ(n, 4); CHECK_EQ(b[1], 3); CHECK_EQ(sbusb_get16(b + 2), 0x0409);
	for (unsigned i = 1; i <= 5; ++i) {
		n = sbusb_desc_string(&id, i, b);
		CHECK(n >= 4 && n == b[0] && b[1] == 3 && (n & 1) == 0);
	}
	CHECK_EQ(sbusb_desc_string(&id, 6, b), 0);
	n = sbusb_desc_string(&id, SBUSB_STR_SERIAL, b);
	CHECK_EQ(n, 2 + 2 * 16);
	CHECK_EQ(b[2], '2'); CHECK_EQ(b[3], 0); CHECK_EQ(b[32], 'e');
	/* the MAC string: 12 upper-case hex digits = the host's address; both addresses locally administered,
	 * unicast, different, and the same on every boot for one serial */
	n = sbusb_desc_string(&id, SBUSB_STR_MAC, b);
	CHECK_EQ(n, 26);
	for (unsigned i = 0; i < 12; ++i) {
		unsigned c = b[2 + 2 * i], nib = (id.host_mac[i / 2] >> ((i & 1) ? 0 : 4)) & 0xf;
		CHECK_EQ(c, "0123456789ABCDEF"[nib]);
		CHECK_EQ(b[3 + 2 * i], 0);
	}
	CHECK_EQ(id.dev_mac[0] & 3, 2); CHECK_EQ(id.host_mac[0] & 3, 2);
	CHECK(memcmp(id.dev_mac, id.host_mac, 6) != 0);
	{
		struct sbusb_ident again, other, none;
		sbusb_ident_init(&again, "0000000000000001");
		sbusb_ident_init(&other, "0000000000000002");
		sbusb_ident_init(&none, 0);
		CHECK(memcmp(&again, &id, sizeof(id)) == 0);
		CHECK(memcmp(other.dev_mac, id.dev_mac, 6) != 0);
		CHECK(strcmp(none.serial, "STARBITE") == 0);
	}
	{       /* an over-long serial is cut, never overruns */
		char longs[100];
		memset(longs, 'x', sizeof(longs) - 1);
		longs[99] = 0;
		sbusb_ident_init(&id, longs);
		CHECK_EQ(strlen(id.serial), 32);
		CHECK_EQ(sbusb_desc_string(&id, SBUSB_STR_SERIAL, b), 66);
	}
	CHECK_EQ(SBUSB_PHYS(0x81), 3); CHECK_EQ(SBUSB_PHYS(0x82), 5); CHECK_EQ(SBUSB_PHYS(0x02), 4);
	return mock_summary("t_desc");
}
