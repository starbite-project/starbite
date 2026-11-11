/* SPDX-License-Identifier: APSL-2.0 */
/* Swap record decoding (see sbd_swap.h); the layout knowledge is SweetFramebuffer.cpp's (ours, 0046..0398). */
#include "sbd_swap.h"

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

bool
sbd_swap_parse(const uint8_t *rec, uint32_t size, struct sbd_swap *out)
{
	uint32_t sh;

	if (rec == 0 || out == 0 || size < SBD_SWAP_REC_MIN) {
		return false;
	}
	sh = (get32(rec, 0x98) != 0 && get32(rec, 0x50) == 0) ? 0x48 : 0;
	out->shift = sh;
	out->swap_id = get32(rec, 0x50 + sh);
	out->mask = get32(rec, 0x104 + sh);
	out->enabled = 0;
	for (uint32_t l = 0; l < SBD_SWAP_LAYERS; l++) {
		struct sbd_swap_layer *y = &out->layer[l];
		uint32_t a = 0x64 + sh + 16 * l, b = 0xc4 + sh + 16 * l;

		y->surface = get32(rec, 0x54 + sh + 4 * l);
		y->sx = (int32_t)get32(rec, a);
		y->sy = (int32_t)get32(rec, a + 4);
		y->sw = (int32_t)get32(rec, a + 8);
		y->sh = (int32_t)get32(rec, a + 12);
		y->dx = (int32_t)get32(rec, b);
		y->dy = (int32_t)get32(rec, b + 4);
		y->dw = (int32_t)get32(rec, b + 8);
		y->dh = (int32_t)get32(rec, b + 12);
		y->enabled = (out->mask & (1u << l)) != 0 && y->surface != 0;
		out->enabled += y->enabled ? 1 : 0;
	}
	return true;
}

bool
sbd_swap_single_full_layer(const struct sbd_swap *s, uint32_t canvas_w, uint32_t canvas_h, uint32_t surface_w,
    uint32_t surface_h)
{
	const struct sbd_swap_layer *y = &s->layer[0];
	int32_t w, h;

	if (s->enabled != 1 || !y->enabled) {
		return false;
	}
	if (surface_w != canvas_w || surface_h != canvas_h) {
		return false;
	}
	/* an empty destination rectangle means "the whole surface" (SweetFramebuffer's reading) */
	w = y->dw > 0 ? y->dw : (int32_t)surface_w;
	h = y->dh > 0 ? y->dh : (int32_t)surface_h;
	if (y->sx > 0 || y->sy > 0 || y->dx > 0 || y->dy > 0) {
		return false;
	}
	return w == (int32_t)canvas_w && h == (int32_t)canvas_h;
}
