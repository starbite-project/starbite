/* SPDX-License-Identifier: GPL-2.0-only */
/* sbguest.ko's shared pieces (sbg_core.c) for the contract-v3 services in their own files: sbnet.c (IP link),
 * sbp9.c (the 9p transport for Starbite's files), sbsnd.c (ALSA PCM). Each service:
 *   - X_init(dev, irq_ok): called once at the end of the probe; does nothing unless
 *     READ_ONCE(hdr->version) >= SBSHM_VERSION_V3 && (READ_ONCE(hdr->host_services) & SBSHM_SVC_X);
 *     irq_ok = the host->guest doorbell interrupt is there (else poll with a timer, like sbblk);
 *   - X_kick(): called from the shared doorbell interrupt (hard irq context, any time, also before X_init or when
 *     the service is off: must be cheap and safe then; defer real work to NAPI / a workqueue).
 * sbg_ring_host() pends the host's doorbell; sbg_vmap_wc() maps a range of the region write-combine. */
#ifndef SBG_CORE_H
#define SBG_CORE_H
#include <linux/device.h>
#include <linux/types.h>
#include "../sbshm.h"

extern phys_addr_t shm_base;
extern u64 shm_size;
extern struct sbshm_hdr *hdr;
void sbg_ring_host(void);
void *sbg_vmap_wc(u64 off, size_t len);

static inline bool sbg_svc(u32 bit)
{
	return READ_ONCE(hdr->version) >= SBSHM_VERSION_V3 && (READ_ONCE(hdr->host_services) & bit);
}

void sbnet_init(struct device *dev, bool irq_ok);
void sbnet_kick(void);
void sbp9_init(struct device *dev, bool irq_ok);
void sbp9_kick(void);
void sbp9_exit(void);			/* the module's exit: unregister the 9p transport */
void sbsnd_init(struct device *dev, bool irq_ok);
void sbsnd_kick(void);
#endif
