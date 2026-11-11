/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB test responder (sbusb_echo.h).
 */
#include "sbusb_echo.h"

static int
same(const uint8_t *a, const uint8_t *b, unsigned n)
{
	for (unsigned i = 0; i < n; ++i) {
		if (a[i] != b[i]) {
			return 0;
		}
	}
	return 1;
}

static void
copy(uint8_t *d, const uint8_t *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; ++i) {
		d[i] = s[i];
	}
}

/* the Internet checksum of p[0..len), ready to store big-endian */
uint16_t
sbusb_inet_sum(const uint8_t *p, uint32_t len)
{
	uint32_t sum = 0;

	for (uint32_t i = 0; i + 1 < len; i += 2) {
		sum += (uint32_t)p[i] << 8 | p[i + 1];
	}
	if ((len & 1u) != 0) {
		sum += (uint32_t)p[len - 1] << 8;
	}
	while ((sum >> 16) != 0) {
		sum = (sum & 0xffffu) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

void
sbusb_echo_ip6(const uint8_t mac[6], uint8_t ip6[16])
{
	for (unsigned i = 0; i < 16; ++i) {
		ip6[i] = 0;
	}
	ip6[0] = 0xfe; ip6[1] = 0x80;
	ip6[8] = mac[0] ^ 0x02u; ip6[9] = mac[1]; ip6[10] = mac[2];
	ip6[11] = 0xff; ip6[12] = 0xfe;
	ip6[13] = mac[3]; ip6[14] = mac[4]; ip6[15] = mac[5];
}

/* ICMPv6 checksum: the pseudo-header (source, destination, length, next header 58) and the message */
static uint16_t
icmp6_sum(const uint8_t *ip6_header, const uint8_t *icmp, uint32_t icmp_len)
{
	uint32_t sum = icmp_len + 58u;

	for (unsigned i = 8; i < 40; i += 2) {
		sum += (uint32_t)ip6_header[i] << 8 | ip6_header[i + 1];
	}
	for (uint32_t i = 0; i + 1 < icmp_len; i += 2) {
		sum += (uint32_t)icmp[i] << 8 | icmp[i + 1];
	}
	if ((icmp_len & 1u) != 0) {
		sum += (uint32_t)icmp[icmp_len - 1] << 8;
	}
	while ((sum >> 16) != 0) {
		sum = (sum & 0xffffu) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static uint32_t
echo6(const uint8_t mac[6], const uint8_t *in, uint32_t len, uint8_t *out, int *kind)
{
	static const uint8_t all_nodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	static const uint8_t unspecified[16] = { 0 };
	uint8_t ours[16], solicited[16];
	const uint8_t *ip = in + 14, *icmp = in + 54;
	uint32_t plen, total;
	uint16_t sum;

	if (len < 14 + 40 + 8 || (ip[0] >> 4) != 6 || ip[6] != 58) {     /* ICMPv6 right after the fixed header */
		return 0;
	}
	plen = (uint32_t)ip[4] << 8 | ip[5];
	if (plen < 8 || 54 + plen > len || same(ip + 8, unspecified, 16) || icmp6_sum(ip, icmp, plen) != 0) {
		return 0;
	}
	sbusb_echo_ip6(mac, ours);
	copy(solicited, all_nodes, 16);                 /* ff02::1:ffXX:XXXX */
	solicited[11] = 1; solicited[12] = 0xff;
	copy(solicited + 13, ours + 13, 3);
	if (icmp[0] == 128 && icmp[1] == 0 && (same(ip + 24, ours, 16) || same(ip + 24, all_nodes, 16))) {
		total = 54 + plen;
		copy(out, in, total);
		out[54] = 129;                          /* echo reply */
		*kind = SBUSB_ECHO_ICMP6;
	} else if (icmp[0] == 135 && icmp[1] == 0 && plen >= 24 && ip[7] == 255 && same(icmp + 8, ours, 16) &&
	    (same(ip + 24, ours, 16) || same(ip + 24, solicited, 16))) {
		total = 54 + 32;
		plen = 32;
		copy(out, in, 54);
		out[54] = 136;                          /* neighbour advertisement */
		out[55] = 0;
		out[58] = 0x60; out[59] = out[60] = out[61] = 0;        /* solicited, override */
		copy(out + 62, ours, 16);
		out[78] = 2; out[79] = 1;               /* option: target link-layer address */
		copy(out + 80, mac, 6);
		out[18] = 0; out[19] = 32;
		*kind = SBUSB_ECHO_ND;
	} else {
		return 0;
	}
	copy(out, in + 6, 6);
	copy(out + 6, mac, 6);
	out[21] = (uint8_t)(icmp[0] == 135 ? 255 : 64);         /* hop limit */
	copy(out + 22, ours, 16);
	copy(out + 38, ip + 8, 16);
	out[56] = out[57] = 0;
	sum = icmp6_sum(out + 14, out + 54, plen);
	out[56] = (uint8_t)(sum >> 8);
	out[57] = (uint8_t)sum;
	return total;
}

uint32_t
sbusb_echo(const uint8_t ip[4], const uint8_t mac[6], const uint8_t *in, uint32_t len, uint8_t *out, uint32_t cap,
    int *kind)
{
	unsigned ethertype;

	*kind = SBUSB_ECHO_NONE;
	if (len < 14 || len > cap || cap < 86) {
		return 0;
	}
	ethertype = (unsigned)in[12] << 8 | in[13];
	if (ethertype == 0x0806u) {
		static const uint8_t request[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };  /* Ethernet, IPv4, 6, 4, request */

		if (len < 42 || !same(in + 14, request, 8) || !same(in + 38, ip, 4)) {
			return 0;
		}
		copy(out, in + 6, 6);                   /* to the asker */
		copy(out + 6, mac, 6);
		copy(out + 12, in + 12, 2);
		copy(out + 14, request, 7);
		out[21] = 2;                            /* reply */
		copy(out + 22, mac, 6);
		copy(out + 28, ip, 4);
		copy(out + 32, in + 22, 10);            /* the asker's MAC and IP */
		*kind = SBUSB_ECHO_ARP;
		return 42;
	}
	if (ethertype == 0x86ddu) {
		return echo6(mac, in, len, out, kind);
	}
	if (ethertype == 0x0800u) {
		uint32_t ihl, total, icmp, icmp_len;
		uint16_t sum;

		if (len < 14 + 20 + 8 || (in[14] >> 4) != 4) {
			return 0;
		}
		ihl = (in[14] & 0xfu) * 4u;
		total = (uint32_t)in[16] << 8 | in[17];
		if (ihl < 20 || total < ihl + 8 || 14 + total > len || in[23] != 1 || !same(in + 30, ip, 4) ||
		    ((in[20] & 0x3fu) != 0 || in[21] != 0)) {           /* not ICMP, not for us, or a fragment */
			return 0;
		}
		icmp = 14 + ihl;
		icmp_len = total - ihl;
		if (in[icmp] != 8 || in[icmp + 1] != 0 || sbusb_inet_sum(in + icmp, icmp_len) != 0) {
			return 0;
		}
		copy(out, in, 14 + total);
		copy(out, in + 6, 6);
		copy(out + 6, mac, 6);
		out[22] = 64;                           /* TTL */
		copy(out + 26, ip, 4);
		copy(out + 30, in + 26, 4);
		out[24] = out[25] = 0;
		sum = sbusb_inet_sum(out + 14, ihl);
		out[24] = (uint8_t)(sum >> 8);
		out[25] = (uint8_t)sum;
		out[icmp] = 0;                          /* echo reply */
		out[icmp + 2] = out[icmp + 3] = 0;
		sum = sbusb_inet_sum(out + icmp, icmp_len);
		out[icmp + 2] = (uint8_t)(sum >> 8);
		out[icmp + 3] = (uint8_t)sum;
		*kind = SBUSB_ECHO_ICMP;
		return 14 + total;
	}
	return 0;
}
