/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd host build (Mac): synthetic capture (capture_synth.c) and a recording injector (inject_rec.c),
 * so the whole sender (session, flow control, encoder, TCP) runs on the Mac for the viewer app and tests.
 */
#ifndef SBL_HOST_H
#define SBL_HOST_H

#include "sender.h"

/* synthetic desktop: scene k is a deterministic image (taskbar, windows, a moving box, a counter) */
void     synth_render(uint32_t k, uint32_t w, uint32_t h, uint8_t *px, size_t stride);
void     synth_set_scene(uint32_t k);    /* the "OS" draws scene k (wakes a waiting capture) */
uint32_t synth_scene(void);
void     synth_set_auto(int fps);        /* > 0: the scene advances by itself at this rate; 0 = manual */
void     synth_set_size(uint32_t w, uint32_t h);   /* initial size (before the capture opens) */
void     synth_get_size(uint32_t *w, uint32_t *h);
uint64_t synth_captures(void);           /* frames the capture has taken from the "OS" */

/* recording injector */
void rec_clear(void);
int  rec_get(sbl_ev *out, int max);      /* copies recorded events, returns how many */
void rec_set_virt(int32_t x, int32_t y, int32_t w, int32_t h);   /* virtual desktop (default: = output) */
void rec_set_print(int on);              /* log every injected event */

#endif
