/* SPDX-License-Identifier: GPL-2.0-only */
/* sbghost's insides for the contract-v3 service files (sbg_net.c, sbg_p9.c, sbg_snd.c). Each service:
 *   sbg_X_load(): from sbg_load, after the v3 regions were zeroed and before CPU_ON, when the header's
 *     host_services is being set: lay out the region (config words, ring indices) if its bit is in sbg_services()
 *     (careful: sbg_services() reads the header, which load writes just before calling these);
 *   sbg_X_poll(): from every sbg_poll while the guest runs: serve the rings, bounded (a few ms at most), then
 *     sbg_ring_guest() when something was produced for the guest.
 * No allocation, no libc beyond what sbghost.c uses (it links into Starbite OS, sbsim and host tests). */
#ifndef SBGHOST_INT_H
#define SBGHOST_INT_H
#include <stdint.h>
#include "sbghost.h"

const struct sbg_os *sbg_os_(void);
const struct sbg_cfg *sbg_cfg_(void);
uintptr_t sbg_shm(uint32_t off);        /* the address of a region offset (host view; non-cacheable on the phone) */
uint32_t sbg_services(void);            /* the header's host_services (0 before v3 is loaded) */
void sbg_ring_guest(void);              /* dmb, then pend the guest's doorbell */

void sbg_net_load(void);
void sbg_net_poll(void);
void sbg_p9_load(void);
void sbg_p9_poll(void);
void sbg_snd_load(void);
void sbg_snd_poll(void);
void sbg_gpu_load(void);
void sbg_gpu_poll(void);
#endif
