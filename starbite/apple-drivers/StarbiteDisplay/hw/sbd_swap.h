/* SPDX-License-Identifier: APSL-2.0 */
/*
 * The IOMobileFramebuffer swap record (selector 5, SwapEnd / swap_submit) as AppleCLCD has decoded it since
 * patch 0046: the fields CoreAnimation fills that this driver uses. Two layouts are known:
 *   iOS 16 (0x46c bytes), macOS 13 (0x468):   swap id 0x50, surfaces 0x54, rects 0x64 / 0xc4, layer mask 0x104
 *   iPadOS 18 (0x514), iOS 26 and macOS 26.5 (0x588): the same fields 0x48 further in
 * The second is told from the first by the swap id: 0x98 set and 0x50 zero.
 */
#ifndef SBD_SWAP_H
#define SBD_SWAP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBD_SWAP_REC_MIN        0x400u          /* the shortest record taken */
#define SBD_SWAP_REC_MAX        0x600u
#define SBD_SWAP_LAYERS         4

struct sbd_swap_layer {
	bool     enabled;               /* in the mask and a surface named */
	uint32_t surface;               /* IOSurface id */
	int32_t  sx, sy, sw, sh;        /* rect a: source */
	int32_t  dx, dy, dw, dh;        /* rect b: destination on the canvas */
};

struct sbd_swap {
	uint32_t swap_id;
	uint32_t mask;
	uint32_t shift;                 /* 0 or 0x48 */
	uint32_t enabled;               /* number of enabled layers */
	struct sbd_swap_layer layer[SBD_SWAP_LAYERS];
};

/* rec must hold SBD_SWAP_REC_MAX bytes (zero-filled past what the client sent). False: size too small. */
bool sbd_swap_parse(const uint8_t *rec, uint32_t size, struct sbd_swap *out);

/* Is this swap one full-canvas layer 0 and nothing else (the case a DECON window can show by address)? */
bool sbd_swap_single_full_layer(const struct sbd_swap *s, uint32_t canvas_w, uint32_t canvas_h,
    uint32_t surface_w, uint32_t surface_h);

#ifdef __cplusplus
}
#endif
#endif /* SBD_SWAP_H */
