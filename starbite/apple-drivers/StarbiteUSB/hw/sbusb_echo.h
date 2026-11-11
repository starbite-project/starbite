/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: a tiny ping responder for the first phone test (PHONE-TEST.md): the phone answers from the
 * driver itself, so the test needs no network stack and no user space on the phone.
 *   IPv6 (the one the test uses: the Mac has a link-local address as soon as the interface is up, nothing to
 *   assign): neighbour solicitation for, and ICMPv6 echo to, the link-local address made from the device MAC
 *   (sbusb_echo_ip6), and echo to ff02::1.
 *   IPv4: ARP for, and ICMP echo to, 169.254.83.66 (works once the Mac has self-assigned a 169.254 address).
 * RFC 826 (ARP), RFC 791/792 (IPv4, ICMP), RFC 8200/4443 (IPv6, ICMPv6), RFC 4861 (ND), RFC 4291 2.5.6 and
 * appendix A (link-local, modified EUI-64), RFC 1071 (checksum).
 */
#ifndef SBUSB_ECHO_H
#define SBUSB_ECHO_H
#include <stdint.h>

#define SBUSB_ECHO_IP   { 169, 254, 83, 66 }    /* link-local: the Mac's self-assigned address reaches it */
#define SBUSB_ECHO_NONE 0
#define SBUSB_ECHO_ARP  1
#define SBUSB_ECHO_ICMP 2
#define SBUSB_ECHO_ND   3       /* neighbour advertisement */
#define SBUSB_ECHO_ICMP6 4

/* fe80::/64 + the modified EUI-64 of mac */
void sbusb_echo_ip6(const uint8_t mac[6], uint8_t ip6[16]);

/* If the frame in[0..len) is one of the requests above, writes the reply frame to out (cap bytes; an echo reply is
 * as long as its request, ARP 42, a neighbour advertisement 86) and returns its length, with *kind set. Else 0. */
uint32_t sbusb_echo(const uint8_t ip[4], const uint8_t mac[6], const uint8_t *in, uint32_t len, uint8_t *out,
    uint32_t cap, int *kind);
uint16_t sbusb_inet_sum(const uint8_t *p, uint32_t len);

#endif
