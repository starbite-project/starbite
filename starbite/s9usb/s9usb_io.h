/* SPDX-License-Identifier: GPL-2.0-only
 * s9usb register access (internal). Device memory: every access goes straight to the register,
 * with the same barriers the loader uses (dmb oshld after reads, dmb oshst before writes).
 */
#ifndef S9USB_IO_H
#define S9USB_IO_H
#include "s9usb.h"

#if defined(__aarch64__)
#define S9_DMB_LD() __asm__ volatile("dmb oshld" : : : "memory")
#define S9_DMB_ST() __asm__ volatile("dmb oshst" : : : "memory")
#define S9_DSB()    __asm__ volatile("dsb sy" : : : "memory")
#else
#define S9_DMB_LD() __asm__ volatile("" : : : "memory")
#define S9_DMB_ST() __asm__ volatile("" : : : "memory")
#define S9_DSB()    __asm__ volatile("" : : : "memory")
#endif

static inline uint32_t s9_rd(const struct s9usb_plat *p, enum s9usb_blk b, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(p->base[b] + off);
    S9_DMB_LD();
    return v;
}

static inline void s9_wr(const struct s9usb_plat *p, enum s9usb_blk b, uint32_t off, uint32_t v)
{
    S9_DMB_ST();
    *(volatile uint32_t *)(p->base[b] + off) = v;
}

static inline void s9_rmw(const struct s9usb_plat *p, enum s9usb_blk b, uint32_t off,
                          uint32_t clear, uint32_t set)
{
    s9_wr(p, b, off, (s9_rd(p, b, off) & ~clear) | set);
}

/* poll until (reg & mask) == want, at most `us` microseconds (1 us steps). 1 on success. */
static inline int s9_wait(const struct s9usb_plat *p, enum s9usb_blk b, uint32_t off,
                          uint32_t mask, uint32_t want, unsigned us)
{
    for (unsigned i = 0; i <= us; ++i) {
        if ((s9_rd(p, b, off) & mask) == want)
            return 1;
        p->udelay(1);
    }
    return 0;
}

#endif
