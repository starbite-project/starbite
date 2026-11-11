/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: CDC-NCM 1.0 framing and class data. 16-bit NTBs only (NTH16 + NDP16, no CRC).
 * NCM 1.0: 3.2 (NTB layout), 3.3 (NDP16), 6.2.1 (NTB parameters), 6.3 (notifications).
 */
#ifndef SBUSB_NCM_H
#define SBUSB_NCM_H
#include <stdint.h>

#define SBUSB_NTB_MAX           16384u          /* dwNtbInMaxSize and dwNtbOutMaxSize */
#define SBUSB_NTB_MIN           2048u           /* the smallest dwNtbInMaxSize a host may set (NCM 1.0 6.2.7) */
#define SBUSB_NTB_DIVISOR       4u
#define SBUSB_NTB_MAXDG         40u             /* datagrams per NTB we build */
#define SBUSB_NTH16_LEN         12u
#define SBUSB_NDP16_LEN(n)      (8u + 4u * ((n) + 1u))
#define SBUSB_NTB_DATA          (SBUSB_NTH16_LEN + SBUSB_NDP16_LEN(SBUSB_NTB_MAXDG))    /* first datagram: 184 */
#define SBUSB_NTH16_SIG         0x484d434eu     /* "NCMH" */
#define SBUSB_NDP16_SIG         0x304d434eu     /* "NCM0" */
#define SBUSB_DG_MIN            14u             /* an Ethernet header */
#define SBUSB_DG_MAX            1514u

/* class requests (bRequest) */
#define SBUSB_REQ_SET_ETH_PACKET_FILTER 0x43u
#define SBUSB_REQ_GET_NTB_PARAMETERS    0x80u
#define SBUSB_REQ_GET_NTB_FORMAT        0x83u
#define SBUSB_REQ_SET_NTB_FORMAT        0x84u
#define SBUSB_REQ_GET_NTB_INPUT_SIZE    0x85u
#define SBUSB_REQ_SET_NTB_INPUT_SIZE    0x86u

/* the 28-byte answer to GetNtbParameters */
unsigned sbusb_ncm_params(uint8_t *out);
/* notifications for the interrupt endpoint: 8 and 16 bytes */
unsigned sbusb_ncm_notify_connection(uint8_t *out, int connected);
unsigned sbusb_ncm_notify_speed(uint8_t *out, uint32_t bits_per_second);

/* ---- building an NTB (to the host) ---- */
struct sbusb_ntb_w {
	uint8_t *buf;
	uint32_t cap;           /* the host's dwNtbInMaxSize, kept within SBUSB_NTB_MIN..SBUSB_NTB_MAX; buf holds SBUSB_NTB_MAX */
	uint32_t len;           /* bytes used so far */
	uint16_t ndg;
	uint16_t seq;
};
void sbusb_ntb_begin(struct sbusb_ntb_w *w, uint8_t *buf, uint32_t cap, uint16_t seq);
/* Room for one datagram of len bytes: where to copy it, or NULL when this NTB cannot take it. */
uint8_t *sbusb_ntb_add(struct sbusb_ntb_w *w, uint32_t len);
/* Headers written; the transfer length. Below cap it is never a multiple of mps (one zero byte is added),
 * so the transfer ends with a short packet and no zero-length packet is needed. */
uint32_t sbusb_ntb_end(struct sbusb_ntb_w *w, unsigned mps);

/* ---- taking an NTB apart (from the host) ---- */
#define SBUSB_NTB_E_SHORT       (-1)    /* shorter than a header */
#define SBUSB_NTB_E_NTH         (-2)    /* bad NTH16 signature, header length or block length */
#define SBUSB_NTB_E_NDP         (-3)    /* NDP16 outside the block, misaligned, bad signature or length */
#define SBUSB_NTB_E_DG          (-4)    /* a datagram outside the block or of an impossible size */
#define SBUSB_NTB_E_LOOP        (-5)    /* too many NDPs */
typedef void (*sbusb_dg_fn)(void *ctx, const uint8_t *frame, uint32_t len);
/* Calls fn for every datagram of the NTB in b[0..len). The count, or a negative error with nothing delivered
 * after the error (datagrams before it were delivered). Never reads outside b[0..len). */
int sbusb_ntb_parse(const uint8_t *b, uint32_t len, sbusb_dg_fn fn, void *ctx);

#endif
