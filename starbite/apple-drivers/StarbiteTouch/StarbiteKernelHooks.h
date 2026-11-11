/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Kernel symbols outside IOKit that StarbiteTouch uses while the drivers that will own them do
 * not exist yet. Each group can be switched off by the integration (-D...=0) when its provider is gone; the
 * driver then does without. All exist in today's build/xnu-12377 (series-12377 through 0425).
 */
#ifndef STARBITE_KERNEL_HOOKS_H
#define STARBITE_KERNEL_HOOKS_H

#include <stdint.h>
#include <stdbool.h>

/* Touch supplies through ACPM (bsd/dev/starlte_touch.c 0302: starlte_pmic_read/write). Until StarbiteACPM. */
#ifndef SB_HAVE_STARLTE_PMIC
#define SB_HAVE_STARLTE_PMIC 1
#endif
/* The test boot's self-stop (bsd/dev/herolte_ufs.c 0423: starlte_sbstop_at/now). */
#ifndef SB_HAVE_SBSTOP
#define SB_HAVE_SBSTOP 1
#endif
/* The drawn pointer layer and the first frame (SweetFramebuffer.cpp 0397, 0388). Until StarbiteDisplay. */
#ifndef SB_HAVE_SWEET_FB
#define SB_HAVE_SWEET_FB 1
#endif
/*
 * Compatibility entry points of today's SweetHID.cpp, defined by StarbiteTouch (sweet_hid_touch and the stall
 * probe's two time stamps) and StarbiteButtons (sweet_hid_key) once SweetHID.cpp is gone, so the legacy callers
 * still link and feed the new services: starlte_touch.c (sweetswipe/sweetscript), herolte_touch.c, a5xelte_*.c,
 * sweet_*.c, SweetFramebuffer.cpp. Set to 0 when those files go too.
 */
#ifndef SB_SWEET_HID_COMPAT
#define SB_SWEET_HID_COMPAT 1
#endif
#ifdef __cplusplus
extern "C" {
#endif
#if SB_HAVE_STARLTE_PMIC
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
#endif
#if SB_HAVE_SBSTOP
int  starlte_sbstop_at(const char *stage);
void starlte_sbstop_now(const char *stage);
#endif
#if SB_HAVE_SWEET_FB
extern volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
extern volatile uint32_t sweet_fb_cur_seq;
extern uint64_t sweet_fb_first_present;
#endif
#ifdef __cplusplus
}
#endif

#endif /* STARBITE_KERNEL_HOOKS_H */
