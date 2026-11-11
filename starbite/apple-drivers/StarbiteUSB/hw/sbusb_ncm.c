/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB CDC-NCM framing (sbusb_ncm.h).
 */
#include "sbusb_ncm.h"
#include "sbusb_io.h"
#include "sbusb_desc.h"

unsigned
sbusb_ncm_params(uint8_t *o)
{
	sbusb_put16(o + 0, 28);                         /* wLength */
	sbusb_put16(o + 2, 0x0001);                     /* bmNtbFormatsSupported: 16-bit only */
	sbusb_put32(o + 4, SBUSB_NTB_MAX);              /* dwNtbInMaxSize */
	sbusb_put16(o + 8, SBUSB_NTB_DIVISOR);          /* wNdpInDivisor */
	sbusb_put16(o + 10, 0);                         /* wNdpInPayloadRemainder */
	sbusb_put16(o + 12, SBUSB_NTB_DIVISOR);         /* wNdpInAlignment */
	sbusb_put16(o + 14, 0);                         /* reserved */
	sbusb_put32(o + 16, SBUSB_NTB_MAX);             /* dwNtbOutMaxSize */
	sbusb_put16(o + 20, SBUSB_NTB_DIVISOR);         /* wNdpOutDivisor */
	sbusb_put16(o + 22, 0);                         /* wNdpOutPayloadRemainder */
	sbusb_put16(o + 24, SBUSB_NTB_DIVISOR);         /* wNdpOutAlignment */
	sbusb_put16(o + 26, 0);                         /* wNtbOutMaxDatagrams: no limit */
	return 28;
}

unsigned
sbusb_ncm_notify_connection(uint8_t *o, int connected)
{
	o[0] = 0xa1; o[1] = 0x00;                       /* NETWORK_CONNECTION */
	sbusb_put16(o + 2, connected ? 1 : 0);
	sbusb_put16(o + 4, SBUSB_IF_CTRL);
	sbusb_put16(o + 6, 0);
	return 8;
}

unsigned
sbusb_ncm_notify_speed(uint8_t *o, uint32_t bps)
{
	o[0] = 0xa1; o[1] = 0x2a;                       /* CONNECTION_SPEED_CHANGE */
	sbusb_put16(o + 2, 0);
	sbusb_put16(o + 4, SBUSB_IF_CTRL);
	sbusb_put16(o + 6, 8);
	sbusb_put32(o + 8, bps);                        /* downlink */
	sbusb_put32(o + 12, bps);                       /* uplink */
	return 16;
}

void
sbusb_ntb_begin(struct sbusb_ntb_w *w, uint8_t *buf, uint32_t cap, uint16_t seq)
{
	w->buf = buf;
	w->cap = cap > SBUSB_NTB_MAX ? SBUSB_NTB_MAX : cap < SBUSB_NTB_MIN ? SBUSB_NTB_MIN : cap;
	w->len = SBUSB_NTB_DATA;
	w->ndg = 0;
	w->seq = seq;
	for (uint32_t i = 0; i < SBUSB_NTB_DATA; ++i) {
		buf[i] = 0;
	}
}

uint8_t *
sbusb_ntb_add(struct sbusb_ntb_w *w, uint32_t len)
{
	uint32_t at = (w->len + SBUSB_NTB_DIVISOR - 1u) & ~(SBUSB_NTB_DIVISOR - 1u);
	uint8_t *e;

	/* one byte kept free for sbusb_ntb_end's short-packet byte */
	if (len < SBUSB_DG_MIN || len > SBUSB_DG_MAX || w->ndg >= SBUSB_NTB_MAXDG ||
	    at + len + 1u > w->cap) {
		return 0;
	}
	for (uint32_t i = w->len; i < at; ++i) {
		w->buf[i] = 0;
	}
	e = w->buf + SBUSB_NTH16_LEN + 8u + 4u * w->ndg;
	sbusb_put16(e, at);
	sbusb_put16(e + 2, len);
	++w->ndg;
	w->len = at + len;
	return w->buf + at;
}

uint32_t
sbusb_ntb_end(struct sbusb_ntb_w *w, unsigned mps)
{
	uint8_t *b = w->buf;

	if (w->len < w->cap && mps != 0 && w->len % mps == 0) {
		b[w->len++] = 0;
	}
	sbusb_put32(b + 0, SBUSB_NTH16_SIG);
	sbusb_put16(b + 4, SBUSB_NTH16_LEN);
	sbusb_put16(b + 6, w->seq);
	sbusb_put16(b + 8, w->len);                     /* wBlockLength */
	sbusb_put16(b + 10, SBUSB_NTH16_LEN);           /* wNdpIndex: right after the header */
	sbusb_put32(b + 12, SBUSB_NDP16_SIG);
	sbusb_put16(b + 16, SBUSB_NDP16_LEN(w->ndg));   /* entries used + the zero terminator */
	sbusb_put16(b + 18, 0);                         /* wNextNdpIndex */
	return w->len;
}

int
sbusb_ntb_parse(const uint8_t *b, uint32_t len, sbusb_dg_fn fn, void *ctx)
{
	uint32_t block, ndp;
	int count = 0;

	if (len < SBUSB_NTH16_LEN) {
		return SBUSB_NTB_E_SHORT;
	}
	if (sbusb_get32(b) != SBUSB_NTH16_SIG || sbusb_get16(b + 4) != SBUSB_NTH16_LEN) {
		return SBUSB_NTB_E_NTH;
	}
	block = sbusb_get16(b + 8);
	if (block == 0) {
		block = len;                            /* "ended by a short packet" */
	}
	if (block < SBUSB_NTH16_LEN || block > len) {
		return SBUSB_NTB_E_NTH;
	}
	ndp = sbusb_get16(b + 10);
	for (unsigned hops = 0; ndp != 0; ++hops) {
		uint32_t ndp_len, entries;

		if (hops >= 16) {
			return SBUSB_NTB_E_LOOP;
		}
		if (ndp < SBUSB_NTH16_LEN || (ndp & 3u) != 0 || ndp + 8u > block) {
			return SBUSB_NTB_E_NDP;
		}
		ndp_len = sbusb_get16(b + ndp + 4);
		if (sbusb_get32(b + ndp) != SBUSB_NDP16_SIG || ndp_len < 16u || (ndp_len & 3u) != 0 ||
		    ndp + ndp_len > block) {
			return SBUSB_NTB_E_NDP;
		}
		entries = (ndp_len - 8u) / 4u;
		for (uint32_t i = 0; i < entries; ++i) {
			uint32_t at = sbusb_get16(b + ndp + 8u + 4u * i);
			uint32_t dl = sbusb_get16(b + ndp + 10u + 4u * i);

			if (at == 0 || dl == 0) {
				break;                  /* the terminator */
			}
			if (at < SBUSB_NTH16_LEN || dl < SBUSB_DG_MIN || dl > SBUSB_DG_MAX || at + dl > block) {
				return SBUSB_NTB_E_DG;
			}
			if (fn != 0) {
				fn(ctx, b + at, dl);
			}
			++count;
		}
		ndp = sbusb_get16(b + ndp + 6);
	}
	return count;
}
