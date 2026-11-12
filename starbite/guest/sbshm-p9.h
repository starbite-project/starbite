/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 * sbp9 (contract v3, sbshm.h SBSHM_P9_OFF, host_services & SBSHM_SVC_P9): Starbite OS's files for the Linux guest,
 * 9P2000.L over shared memory. The guest's kernel (sbguest.ko: sbp9.c) is a 9p transport named "sbshm":
 *     mount -t 9p -o trans=sbshm,version=9p2000.L,msize=131072 starbite /home/user/Starbite
 * and Starbite OS (sbghost: host/sbg_p9.c) is the 9P server over a struct sbg_fs (host/sbg_p9.h).
 *
 * The channel: SBSHM_P9_SLOTS message slots of SBSHM_P9_SLOT bytes each (= the largest msize) at SBSHM_P9_SLOTS_OFF,
 * and two single-producer single-consumer rings of small entries in the same style as sbblk:
 *   req:  guest -> host (the guest produces: req_head; the host consumes: req_tail)
 *   done: host -> guest (the host produces: done_head; the guest consumes: done_tail)
 * One slot carries one exchange: the guest writes the whole T-message into a free slot, then the entry {id, slot,
 * len = the T-message's bytes}, dmb, req_head + 1, and pends the host's doorbell (hdr.db_to_host). The host copies
 * the entry, reads the T-message, and writes the R-message INTO THE SAME SLOT (over the T-message: the host has
 * taken what it needs first), then the entry {id, slot, len = the R-message's bytes, status 0}, dmb, req_tail and
 * done_head + 1; one doorbell (hdr.db_to_guest) per batch. The guest copies the R-message out, frees the slot,
 * dmb, done_tail + 1. A slot is the guest's again only after its completion was reaped, so with at most
 * SBSHM_P9_SLOTS exchanges in flight the rings (SBSHM_P9_RING entries) never overflow. The host answers in ring order.
 * A malformed entry (a slot out of range, a length the slot cannot hold, a T-message whose size word disagrees)
 * completes with len 0 and status -EIO (-SBG_EIO): the guest fails that request with EIO.
 *
 * 9P itself is unchanged (little-endian, size[4] type[1] tag[2] ...): the tag is the guest's, the id is the
 * transport's (slot | generation << 8, echoed in the completion). Tversion's msize is at most SBSHM_P9_SLOT.
 * The host fills the config words before CPU_ON; flags & SBSHM_P9_RO: the files are read-only (every change answers
 * EROFS; the guest mounts it -o ro). Each index has its own 64-byte line; the region is non-cacheable on both sides:
 * plain loads / stores, byte-wise where 9P's fields are unaligned (sbsim runs with the MMU off: Device memory).
 */
#ifndef SBSHM_P9_H
#define SBSHM_P9_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define SBSHM_P9_MAGIC      0x39504253U         /* "SBP9" */
#define SBSHM_P9_SIZE       0x200000U           /* the channel's share of the region: 2 MiB */
#define SBSHM_P9_SLOT       0x20000U            /* 128 KiB per slot: the largest msize */
#define SBSHM_P9_SLOTS      15U                 /* exchanges in flight */
#define SBSHM_P9_RING       16U                 /* entries per ring, power of two, >= SBSHM_P9_SLOTS */
#define SBSHM_P9_SLOTS_OFF  0x10000U            /* from SBSHM_P9_OFF: slot i at + i * SBSHM_P9_SLOT */

enum { SBSHM_P9_RO = 1U << 0 };                 /* sbshm_p9.flags */

struct sbshm_p9_ent {
    uint32_t id;                    /* the guest's: slot | generation << 8, echoed */
    uint32_t slot;                  /* 0 .. SBSHM_P9_SLOTS - 1 */
    uint32_t len;                   /* req: the T-message's bytes; done: the R-message's bytes (0 with status < 0) */
    int32_t status;                 /* done: 0, or a negative Linux errno (the exchange failed: no R-message) */
};                                  /* 16 bytes */

struct sbshm_p9 {
    /* ---- host writes before CPU_ON ---- */
    uint32_t magic;                 /* SBSHM_P9_MAGIC: the channel is laid out */
    uint32_t msize;                 /* SBSHM_P9_SLOT */
    uint32_t slots;                 /* SBSHM_P9_SLOTS */
    uint32_t flags;                 /* SBSHM_P9_* */
    char name[32];                  /* what the host serves, for logs ("Starbite card"), NUL-terminated */
    uint32_t pad0[4];               /* to 64 bytes */
    /* ---- ring indices, one cache line each ---- */
    volatile uint32_t req_head;     /* guest */
    uint32_t pad1[15];
    volatile uint32_t req_tail;     /* host */
    uint32_t pad2[15];
    volatile uint32_t done_head;    /* host */
    uint32_t pad3[15];
    volatile uint32_t done_tail;    /* guest */
    uint32_t pad4[15];
    struct sbshm_p9_ent req[SBSHM_P9_RING];
    struct sbshm_p9_ent done[SBSHM_P9_RING];
};                                  /* 832 bytes, then the slots at SBSHM_P9_SLOTS_OFF */

#endif
