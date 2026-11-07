/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's hw.h, reduced to what its USB drivers (usb_dwc3.c, usb_phy.c, max77705_muic.c, copied
 * from shim/starlte) need, on libsb's timer. */
#pragma once
#include <stdint.h>
#include "sb.h"

static inline uint64_t hw_ticks(void) { return sb_ticks(); }
static inline uint64_t hw_freq(void) { return sb_freq(); }
static inline void hw_wait_us(uint64_t us) { sb_udelay(us); }
static inline void hw_wait_ms(unsigned ms) { sb_mdelay(ms); }
