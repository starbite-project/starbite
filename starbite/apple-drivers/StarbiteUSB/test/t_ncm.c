/* SPDX-License-Identifier: APSL-2.0
 * Host test: NTB16 build / parse round trips, the parser against broken blocks, the class data, the test
 * responder.
 */
#include "mock.h"
#include "../hw/sbusb_echo.h"

static struct { unsigned n; uint32_t len[64]; uint8_t first[64], last[64]; const uint8_t *at[64]; } got;

static void
take(void *ctx, const uint8_t *frame, uint32_t len)
{
	(void)ctx;
	if (got.n < 64) {
		got.len[got.n] = len;
		got.first[got.n] = frame[0];
		got.last[got.n] = frame[len - 1];
		got.at[got.n] = frame;
	}
	++got.n;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

static void
round_trip(unsigned count, uint32_t cap, unsigned mps, uint32_t fixed_len)
{
	static uint8_t ntb[SBUSB_NTB_MAX + 64];
	struct sbusb_ntb_w w;
	uint32_t lens[64], total;
	unsigned put = 0;

	memset(ntb, 0xee, sizeof(ntb));
	sbusb_ntb_begin(&w, ntb, cap, 0x1234);
	for (unsigned i = 0; i < count; ++i) {
		uint32_t len = fixed_len ? fixed_len : 14 + rnd() % (1514 - 14 + 1);
		uint8_t *p = sbusb_ntb_add(&w, len);
		if (p == 0) {
			break;
		}
		CHECK_EQ((uintptr_t)(p - ntb) % 4, 0);          /* wNdpInDivisor 4, remainder 0 */
		memset(p, (int)(0x40 + put), len);
		p[0] = (uint8_t)put;
		p[len - 1] = (uint8_t)(put ^ 0xff);
		lens[put++] = len;
	}
	total = sbusb_ntb_end(&w, mps);
	CHECK(total <= (cap < SBUSB_NTB_MIN ? SBUSB_NTB_MIN : cap));
	cap = cap < SBUSB_NTB_MIN ? SBUSB_NTB_MIN : cap;
	CHECK(total == cap || total % mps != 0);                /* always ends in a short packet below the maximum */
	CHECK_EQ(ntb[total], 0xee);                             /* nothing written past the end */
	CHECK_EQ(sbusb_get32(ntb), 0x484d434e);                 /* "NCMH" */
	CHECK_EQ(sbusb_get16(ntb + 4), 12);
	CHECK_EQ(sbusb_get16(ntb + 6), 0x1234);
	CHECK_EQ(sbusb_get16(ntb + 8), total);
	CHECK_EQ(sbusb_get16(ntb + 10) % 4, 0);
	CHECK_EQ(sbusb_get32(ntb + sbusb_get16(ntb + 10)), 0x304d434e);         /* "NCM0" */
	CHECK_EQ(sbusb_get16(ntb + sbusb_get16(ntb + 10) + 4), 8 + 4 * (put + 1));
	memset(&got, 0, sizeof(got));
	CHECK_EQ(sbusb_ntb_parse(ntb, total, take, 0), (int)put);
	CHECK_EQ(got.n, put);
	for (unsigned i = 0; i < put && i < 64; ++i) {
		CHECK_EQ(got.len[i], lens[i]);
		CHECK_EQ(got.first[i], i);
		CHECK_EQ(got.last[i], (uint8_t)(i ^ 0xff));
	}
}

static void
bad_blocks(void)
{
	static uint8_t ntb[4096], copy[4096];
	struct sbusb_ntb_w w;
	uint32_t total;

	sbusb_ntb_begin(&w, ntb, sizeof(ntb), 1);
	memset(sbusb_ntb_add(&w, 100), 1, 100);
	memset(sbusb_ntb_add(&w, 60), 2, 60);
	total = sbusb_ntb_end(&w, 512);
	CHECK_EQ(sbusb_ntb_parse(ntb, total, 0, 0), 2);
	CHECK_EQ(sbusb_ntb_parse(ntb, 11, 0, 0), SBUSB_NTB_E_SHORT);
	CHECK_EQ(sbusb_ntb_parse(ntb, total - 1, 0, 0), SBUSB_NTB_E_NTH);      /* block longer than what arrived */
#define BREAK(off, val16, want) do { memcpy(copy, ntb, sizeof(ntb)); sbusb_put16(copy + (off), (val16)); \
	CHECK_EQ(sbusb_ntb_parse(copy, total, take, 0), (want)); } while (0)
	BREAK(0, 0x4d43, SBUSB_NTB_E_NTH);              /* signature */
	BREAK(4, 16, SBUSB_NTB_E_NTH);                  /* header length */
	BREAK(8, 8, SBUSB_NTB_E_NTH);                   /* block length below the header */
	BREAK(10, 8, SBUSB_NTB_E_NDP);                  /* NDP inside the header */
	BREAK(10, 14, SBUSB_NTB_E_NDP);                 /* NDP misaligned */
	BREAK(10, (uint16_t)(total - 4), SBUSB_NTB_E_NDP);      /* NDP header past the end */
	BREAK(12, 0x4d42, SBUSB_NTB_E_NDP);             /* NDP signature */
	BREAK(16, 12, SBUSB_NTB_E_NDP);                 /* NDP length below 16 */
	BREAK(16, 18, SBUSB_NTB_E_NDP);                 /* NDP length not a multiple of 4 */
	BREAK(16, 0xfff0, SBUSB_NTB_E_NDP);             /* NDP past the block */
	BREAK(20, 4, SBUSB_NTB_E_DG);                   /* datagram inside the header */
	BREAK(20, (uint16_t)(total - 50), SBUSB_NTB_E_DG);      /* datagram past the block */
	BREAK(22, 13, SBUSB_NTB_E_DG);                  /* shorter than an Ethernet header */
	BREAK(22, 1515, SBUSB_NTB_E_DG);                /* longer than a frame */
	BREAK(18, 12, SBUSB_NTB_E_LOOP);                /* wNextNdpIndex pointing at itself */
	BREAK(8, 0, 2);                                 /* wBlockLength 0: ended by the short packet */
	BREAK(10, 0, 0);                                /* no NDP at all: an empty block */
#undef BREAK
	/* a second NDP chained after the first is followed */
	memcpy(copy, ntb, sizeof(ntb));
	{
		uint32_t ndp2 = (total + 3) & ~3u;
		sbusb_put16(copy + 18, ndp2);
		sbusb_put32(copy + ndp2, 0x304d434e);
		sbusb_put16(copy + ndp2 + 4, 16);
		sbusb_put16(copy + ndp2 + 6, 0);
		sbusb_put16(copy + ndp2 + 8, sbusb_get16(ntb + 20));
		sbusb_put16(copy + ndp2 + 10, 20);
		sbusb_put32(copy + ndp2 + 12, 0);
		sbusb_put16(copy + 8, ndp2 + 16);
		CHECK_EQ(sbusb_ntb_parse(copy, ndp2 + 16, 0, 0), 3);
	}
	/* random and bit-flipped blocks: never a read outside the block (ASan), never a datagram outside it */
	for (unsigned round = 0; round < 20000; ++round) {
		uint32_t len = rnd() % 600;
		uint8_t *exact = malloc(len ? len : 1);         /* an exact-size heap block: ASan sees any over-read */
		memcpy(copy, ntb, sizeof(ntb));
		for (unsigned k = 0; k < 1 + rnd() % 4; ++k) {
			copy[rnd() % 40] = (uint8_t)rnd();
		}
		if (round & 1) {
			for (uint32_t i = 0; i < len; ++i) {
				copy[i] = (uint8_t)rnd();
			}
			sbusb_put32(copy, 0x484d434e);
			sbusb_put16(copy + 4, 12);
		}
		memcpy(exact, copy, len);
		memset(&got, 0, sizeof(got));
		(void)sbusb_ntb_parse(exact, len, take, 0);
		for (unsigned i = 0; i < got.n && i < 64; ++i) {
			CHECK(got.at[i] >= exact && got.at[i] + got.len[i] <= exact + len);
		}
		free(exact);
	}
}

static void
class_data(void)
{
	uint8_t b[32];

	memset(b, 0xee, sizeof(b));
	CHECK_EQ(sbusb_ncm_params(b), 28);
	CHECK_EQ(sbusb_get16(b), 28);
	CHECK_EQ(sbusb_get16(b + 2), 1);                /* 16-bit NTBs only */
	CHECK_EQ(sbusb_get32(b + 4), 16384);
	CHECK_EQ(sbusb_get16(b + 8), 4); CHECK_EQ(sbusb_get16(b + 10), 0); CHECK_EQ(sbusb_get16(b + 12), 4);
	CHECK_EQ(sbusb_get16(b + 14), 0);
	CHECK_EQ(sbusb_get32(b + 16), 16384);
	CHECK_EQ(sbusb_get16(b + 20), 4); CHECK_EQ(sbusb_get16(b + 22), 0); CHECK_EQ(sbusb_get16(b + 24), 4);
	CHECK_EQ(sbusb_get16(b + 26), 0);
	CHECK_EQ(b[28], 0xee);
	/* NCM 1.0 table 6-3 limits: remainder < divisor, alignment a power of two >= 4, sizes >= 2048 */
	CHECK(sbusb_get16(b + 10) < sbusb_get16(b + 8));
	CHECK(sbusb_get32(b + 4) >= 2048 && sbusb_get32(b + 16) >= 2048);
	CHECK(SBUSB_NTB_MAX % 512 == 0 && SBUSB_NTB_MAX % 64 == 0);     /* an OUT TRB is whole packets */
	CHECK_EQ(SBUSB_NTB_DATA, 184);

	CHECK_EQ(sbusb_ncm_notify_connection(b, 1), 8);
	CHECK(memcmp(b, "\xa1\x00\x01\x00\x00\x00\x00\x00", 8) == 0);
	CHECK_EQ(sbusb_ncm_notify_connection(b, 0), 8);
	CHECK(memcmp(b, "\xa1\x00\x00\x00\x00\x00\x00\x00", 8) == 0);
	CHECK_EQ(sbusb_ncm_notify_speed(b, 480000000), 16);
	CHECK(memcmp(b, "\xa1\x2a\x00\x00\x00\x00\x08\x00", 8) == 0);
	CHECK_EQ(sbusb_get32(b + 8), 480000000); CHECK_EQ(sbusb_get32(b + 12), 480000000);
}

/* the ICMPv6 checksum computed independently of the driver: pseudo-header bytes laid out, then RFC 1071 */
static uint16_t
ref_sum6(const uint8_t *frame, uint32_t icmp_len)
{
	uint8_t b[2048];
	memcpy(b, frame + 22, 32);
	b[32] = 0; b[33] = 0; b[34] = (uint8_t)(icmp_len >> 8); b[35] = (uint8_t)icmp_len;
	b[36] = b[37] = b[38] = 0; b[39] = 58;
	memcpy(b + 40, frame + 54, icmp_len);
	return sbusb_inet_sum(b, 40 + icmp_len);
}

static void
echo_v6(void)
{
	static const uint8_t ip4[4] = SBUSB_ECHO_IP, mac[6] = { 0x02, 0x53, 0x42, 0xa1, 0xb2, 0xc4 };
	static const uint8_t hostmac[6] = { 0x02, 0x53, 0x42, 0xa1, 0xb2, 0xc5 };
	static const uint8_t want[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0x53, 0x42, 0xff, 0xfe, 0xa1, 0xb2, 0xc4 };
	uint8_t ours[16], host[16], req[200], out[200];
	int kind;
	uint32_t n;
	uint16_t s;

	sbusb_echo_ip6(mac, ours);
	CHECK(memcmp(ours, want, 16) == 0);             /* fe80::53:42ff:fea1:b2c4 */
	sbusb_echo_ip6(hostmac, host);

	/* neighbour solicitation for our address, to the solicited-node group, as the Mac sends before a ping */
	memset(req, 0, sizeof(req));
	memcpy(req, "\x33\x33\xff\xa1\xb2\xc4", 6); memcpy(req + 6, hostmac, 6); req[12] = 0x86; req[13] = 0xdd;
	req[14] = 0x60; req[19] = 32; req[20] = 58; req[21] = 255;
	memcpy(req + 22, host, 16);
	memcpy(req + 38, "\xff\x02\0\0\0\0\0\0\0\0\0\x01\xff\xa1\xb2\xc4", 16);
	req[54] = 135; memcpy(req + 62, ours, 16); req[78] = 1; req[79] = 1; memcpy(req + 80, hostmac, 6);
	s = ref_sum6(req, 32); req[56] = (uint8_t)(s >> 8); req[57] = (uint8_t)s;
	n = sbusb_echo(ip4, mac, req, 86, out, sizeof(out), &kind);
	CHECK_EQ(n, 86); CHECK_EQ(kind, SBUSB_ECHO_ND);
	CHECK(memcmp(out, hostmac, 6) == 0 && memcmp(out + 6, mac, 6) == 0);
	CHECK_EQ(out[14] >> 4, 6); CHECK_EQ(out[19], 32); CHECK_EQ(out[20], 58);
	CHECK_EQ(out[21], 255);                         /* RFC 4861: hop limit 255 or it is ignored */
	CHECK(memcmp(out + 22, ours, 16) == 0 && memcmp(out + 38, host, 16) == 0);
	CHECK_EQ(out[54], 136); CHECK_EQ(out[58], 0x60);
	CHECK(memcmp(out + 62, ours, 16) == 0);
	CHECK_EQ(out[78], 2); CHECK_EQ(out[79], 1); CHECK(memcmp(out + 80, mac, 6) == 0);
	CHECK_EQ(ref_sum6(out, 32), 0);
	req[77] ^= 1;                                   /* a solicitation for another address */
	s = 0; req[56] = req[57] = 0; s = ref_sum6(req, 32); req[56] = (uint8_t)(s >> 8); req[57] = (uint8_t)s;
	CHECK_EQ(sbusb_echo(ip4, mac, req, 86, out, sizeof(out), &kind), 0);
	req[77] ^= 1; req[21] = 64;                     /* not from this link */
	req[56] = req[57] = 0; s = ref_sum6(req, 32); req[56] = (uint8_t)(s >> 8); req[57] = (uint8_t)s;
	CHECK_EQ(sbusb_echo(ip4, mac, req, 86, out, sizeof(out), &kind), 0);

	/* echo request to our address (ping6), 16 data bytes, and to ff02::1 */
	for (unsigned pass = 0; pass < 2; ++pass) {
		memset(req, 0, sizeof(req));
		memcpy(req, mac, 6); memcpy(req + 6, hostmac, 6); req[12] = 0x86; req[13] = 0xdd;
		req[14] = 0x60; req[19] = 24; req[20] = 58; req[21] = 64;
		memcpy(req + 22, host, 16);
		if (pass == 0) {
			memcpy(req + 38, ours, 16);
		} else {
			req[38] = 0xff; req[39] = 0x02; req[53] = 1;
		}
		req[54] = 128; req[58] = 0x12; req[59] = 0x34; req[61] = 9;
		for (unsigned i = 0; i < 16; ++i) { req[62 + i] = (uint8_t)(0x80 + i); }
		s = ref_sum6(req, 24); req[56] = (uint8_t)(s >> 8); req[57] = (uint8_t)s;
		n = sbusb_echo(ip4, mac, req, 78, out, sizeof(out), &kind);
		CHECK_EQ(n, 78); CHECK_EQ(kind, SBUSB_ECHO_ICMP6);
		CHECK(memcmp(out, hostmac, 6) == 0 && memcmp(out + 6, mac, 6) == 0);
		CHECK(memcmp(out + 22, ours, 16) == 0 && memcmp(out + 38, host, 16) == 0);     /* from us, even for ff02::1 */
		CHECK_EQ(out[54], 129);
		CHECK(memcmp(out + 58, req + 58, 20) == 0);
		CHECK_EQ(ref_sum6(out, 24), 0);
	}
	req[70] ^= 1;                                   /* corrupt */
	CHECK_EQ(sbusb_echo(ip4, mac, req, 78, out, sizeof(out), &kind), 0);
	req[70] ^= 1;
	CHECK_EQ(sbusb_echo(ip4, mac, req, 77, out, sizeof(out), &kind), 0);   /* cut short */
	req[20] = 17;                                   /* UDP */
	CHECK_EQ(sbusb_echo(ip4, mac, req, 78, out, sizeof(out), &kind), 0);
	req[20] = 58; memset(req + 22, 0, 16);          /* from the unspecified address: nobody to answer */
	req[56] = req[57] = 0; s = ref_sum6(req, 24); req[56] = (uint8_t)(s >> 8); req[57] = (uint8_t)s;
	CHECK_EQ(sbusb_echo(ip4, mac, req, 78, out, sizeof(out), &kind), 0);
}

static void
echo(void)
{
	static const uint8_t ip[4] = SBUSB_ECHO_IP, mac[6] = { 2, 0x53, 0x42, 1, 2, 4 };
	static const uint8_t hostmac[6] = { 2, 0x53, 0x42, 1, 2, 5 }, hostip[4] = { 169, 254, 10, 20 };
	uint8_t req[128], out[128];
	int kind;
	uint32_t n;

	/* ARP: who has 169.254.83.66, tell 169.254.10.20 */
	memset(req, 0, sizeof(req));
	memset(req, 0xff, 6); memcpy(req + 6, hostmac, 6);
	memcpy(req + 12, "\x08\x06\x00\x01\x08\x00\x06\x04\x00\x01", 10);
	memcpy(req + 22, hostmac, 6); memcpy(req + 28, hostip, 4); memcpy(req + 38, ip, 4);
	n = sbusb_echo(ip, mac, req, 60, out, sizeof(out), &kind);     /* padded to 60 as Ethernet would */
	CHECK_EQ(n, 42); CHECK_EQ(kind, SBUSB_ECHO_ARP);
	CHECK(memcmp(out, hostmac, 6) == 0 && memcmp(out + 6, mac, 6) == 0);
	CHECK(memcmp(out + 12, "\x08\x06\x00\x01\x08\x00\x06\x04\x00\x02", 10) == 0);
	CHECK(memcmp(out + 22, mac, 6) == 0 && memcmp(out + 28, ip, 4) == 0);
	CHECK(memcmp(out + 32, hostmac, 6) == 0 && memcmp(out + 38, hostip, 4) == 0);
	req[41] ^= 1;                                   /* for someone else */
	CHECK_EQ(sbusb_echo(ip, mac, req, 60, out, sizeof(out), &kind), 0);
	req[41] ^= 1; req[21] = 2;                      /* a reply, not a request */
	CHECK_EQ(sbusb_echo(ip, mac, req, 60, out, sizeof(out), &kind), 0);

	/* ICMP echo request, 56 data bytes, like `ping` */
	memset(req, 0, sizeof(req));
	memcpy(req, mac, 6); memcpy(req + 6, hostmac, 6); req[12] = 8; req[13] = 0;
	req[14] = 0x45; req[16] = 0; req[17] = 84; req[18] = 0x12; req[19] = 0x34; req[22] = 64; req[23] = 1;
	memcpy(req + 26, hostip, 4); memcpy(req + 30, ip, 4);
	{ uint16_t s = sbusb_inet_sum(req + 14, 20); req[24] = (uint8_t)(s >> 8); req[25] = (uint8_t)s; }
	req[34] = 8; req[38] = 0xab; req[39] = 0xcd; req[40] = 0; req[41] = 7;
	for (unsigned i = 0; i < 56; ++i) { req[42 + i] = (uint8_t)i; }
	{ uint16_t s = sbusb_inet_sum(req + 34, 64); req[36] = (uint8_t)(s >> 8); req[37] = (uint8_t)s; }
	CHECK_EQ(sbusb_inet_sum(req + 14, 20), 0);
	n = sbusb_echo(ip, mac, req, 98, out, sizeof(out), &kind);
	CHECK_EQ(n, 98); CHECK_EQ(kind, SBUSB_ECHO_ICMP);
	CHECK(memcmp(out, hostmac, 6) == 0 && memcmp(out + 6, mac, 6) == 0);
	CHECK(memcmp(out + 26, ip, 4) == 0 && memcmp(out + 30, hostip, 4) == 0);
	CHECK_EQ(sbusb_inet_sum(out + 14, 20), 0);      /* a valid IP header checksum */
	CHECK_EQ(out[34], 0);                           /* echo reply */
	CHECK_EQ(sbusb_inet_sum(out + 34, 64), 0);      /* a valid ICMP checksum */
	CHECK(memcmp(out + 38, req + 38, 60) == 0);     /* identifier, sequence, data returned */
	req[60] ^= 1;                                   /* a corrupt request is not answered */
	CHECK_EQ(sbusb_echo(ip, mac, req, 98, out, sizeof(out), &kind), 0);
	req[60] ^= 1;
	CHECK_EQ(sbusb_echo(ip, mac, req, 97, out, sizeof(out), &kind), 0);    /* cut short */
	CHECK_EQ(sbusb_echo(ip, mac, req, 98, out, 90, &kind), 0);             /* no room for the reply */
	req[23] = 17;                                   /* UDP */
	CHECK_EQ(sbusb_echo(ip, mac, req, 98, out, sizeof(out), &kind), 0);
	echo_v6();
}

int
main(void)
{
	round_trip(1, SBUSB_NTB_MAX, 512, 64);
	round_trip(1, SBUSB_NTB_MAX, 512, 1514);
	round_trip(1, SBUSB_NTB_MAX, 512, 14);
	round_trip(1, SBUSB_NTB_MAX, 512, 512 - 184);           /* lands on a packet boundary: one byte added */
	round_trip(1, SBUSB_NTB_MAX, 64, 64 * 4 - 184);
	round_trip(40, SBUSB_NTB_MAX, 512, 60);                 /* the datagram table full */
	round_trip(41, SBUSB_NTB_MAX, 512, 60);                 /* one too many: 40 go */
	round_trip(64, SBUSB_NTB_MAX, 512, 1514);               /* the block full: 10 go */
	round_trip(64, SBUSB_NTB_MIN, 512, 1514);               /* the smallest block a host may ask for */
	round_trip(3, 100, 512, 1000);                          /* a cap below the minimum counts as 2048: one fits */
	for (unsigned i = 0; i < 2000; ++i) {
		static const uint32_t caps[4] = { SBUSB_NTB_MAX, 8192, 4096, SBUSB_NTB_MIN };
		round_trip(1 + rnd() % 48, caps[rnd() % 4], (rnd() & 1) ? 512 : 64, 0);
	}
	bad_blocks();
	class_data();
	echo();
	return mock_summary("t_ncm");
}
