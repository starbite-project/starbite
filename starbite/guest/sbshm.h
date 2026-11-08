/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.9 app support: the shared-memory contract between Starbite OS (the "host": the four A55
 * cores, EL1) and the guest Linux kernel on the four M3 cores (also EL1; no hypervisor, see
 * docs/STARBITE-OS-LINUX-M3.md). Freestanding C, used by both sides:
 *   - Starbite OS / the phone probe / sbsim (the QEMU stand-in for Starbite OS): starbite/guest/{probe,sbsim}
 *   - the guest: guestd (Linux user space, /dev/mem) and sbguest.ko (shutdown, doorbells)
 *
 * Memory (one profile per machine, physical addresses):
 *   LINUX_BASE .. +LINUX_SIZE   the guest's RAM: the only /memory in its device tree. Image at +0 (2 MiB
 *                               aligned, text_offset 0), DTB at +SBG_DTB_OFF, initrd at +SBG_RD_OFF.
 *   SHM_BASE   .. +SHM_SIZE     shared, mapped non-cacheable on both sides (Starbite: its NC map of the
 *                               high bank; Linux: a reserved-memory node without no-map + /dev/mem O_SYNC
 *                               = write-combine). Nothing here depends on the two clusters being coherent.
 *
 * Every field has one writer. A writer stores the payload, then `dmb sy`, then the index / sequence word;
 * a reader loads the index, `dmb sy`, then the payload. Rings are single-producer single-consumer.
 */
#ifndef SBSHM_H
#define SBSHM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ---- profiles -------------------------------------------------------------------------------------- */
/* S9 (Exynos 9810): the 2 GiB high bank 0x8_8000_0000..0x8_FFFF_FFFF (TWRP /proc/iomem; no DT reserved node
 * in it). Starbite OS keeps 0x8_F000_0000 + 32 MiB for the Mali page tables (libsb SB_GPU_MEM); the rest of
 * 0x8_E400_0000..0x8_FFFF_FFFF stays Starbite's. */
#define SBG_S9_LINUX_BASE   0x880000000ULL
#define SBG_S9_LINUX_SIZE   0x060000000ULL      /* 1.5 GiB */
#define SBG_S9_SHM_BASE     0x8e0000000ULL
#define SBG_S9_SHM_SIZE     0x004000000ULL      /* 64 MiB */
#define SBG_S9_GICD         0x10101000ULL       /* GIC-400 */
#define SBG_S9_GICC         0x10102000ULL

/* QEMU virt (gic-version=2, -m 2048: RAM 0x4000_0000..0xBFFF_FFFF); the "store" (what Starbite OS reads
 * from UFS on the phone) is put in RAM by -device loader at SBG_QEMU_STORE. */
#define SBG_QEMU_LINUX_BASE 0x080000000ULL
#define SBG_QEMU_LINUX_SIZE 0x03c000000ULL      /* 960 MiB */
#define SBG_QEMU_SHM_BASE   0x0bc000000ULL
#define SBG_QEMU_SHM_SIZE   0x004000000ULL
#define SBG_QEMU_GICD       0x08000000ULL
#define SBG_QEMU_GICC       0x08010000ULL
#define SBG_QEMU_STORE      0x048000000ULL      /* manifest page, then Image, DTB, initrd */

/* inside the guest's RAM */
#define SBG_DTB_OFF         0x08000000ULL       /* +128 MiB: past any Image + BSS */
#define SBG_RD_OFF          0x08200000ULL

/* ---- inside the shared region ---------------------------------------------------------------------- */
#define SBSHM_HDR_OFF       0x000000U           /* struct sbshm_hdr (4 KiB page) */
#define SBSHM_TRAMP_OFF     0x000f00U           /* the M3 entry trampoline (sbg_tramp, 64 bytes) */
#define SBSHM_H2G_OFF       0x001000U           /* ring: host -> guest (control + input, in order) */
#define SBSHM_G2H_OFF       0x003000U           /* ring: guest -> host */
#define SBSHM_OOPS_OFF      0x100000U           /* pstore/ramoops console zone (the guest's kernel log) */
#define SBSHM_OOPS_SIZE     0x080000U           /* 512 KiB */
#define SBSHM_SURF_OFF      0x200000U           /* surface buffers, each surf_stride * surf_h */

#define SBSHM_MAGIC         0x31764d4853425353ULL   /* "SSBSHMv1" */
#define SBSHM_VERSION       1U

/* host_state (Starbite) */
enum {
    SBG_HOST_OFF = 0,       /* M3 cluster off, nothing loaded */
    SBG_HOST_LOADED = 1,    /* Image / DTB / initrd in place, header written */
    SBG_HOST_STARTED = 2,   /* CPU_ON of the first M3 core accepted */
    SBG_HOST_STOPPING = 3,  /* MSG_STOP sent; waiting for every M3 core to be OFF (PSCI AFFINITY_INFO) */
};
/* guest_state (Linux) */
enum {
    SBG_GUEST_NONE = 0,
    SBG_GUEST_UP = 1,       /* guestd started (the kernel booted, user space runs) */
    SBG_GUEST_RUNNING = 2,  /* surface and input ready */
    SBG_GUEST_STOPPING = 3, /* user space syncing / powering off */
    SBG_GUEST_OFF = 4,      /* sbguest.ko: the last CPU is in PSCI CPU_OFF */
    SBG_GUEST_PANIC = 5,    /* sbguest.ko restart handler from a panic: others may still be parked */
};

#define SBSHM_FMT_XRGB8888  1U                  /* little-endian B, G, R, X (DRM_FORMAT_XRGB8888) */

struct sbshm_hdr {
    uint64_t magic;
    uint32_t version;
    uint32_t hdr_size;
    /* ---- written by the host (Starbite OS) before CPU_ON, then only host_* words ---- */
    uint32_t host_state;
    uint32_t boot_count;            /* +1 per guest start */
    uint64_t host_time_ms;          /* Starbite's clock, updated every tick */
    uint64_t gicd_base;             /* physical, for the guest's doorbell writes (GICD_ISPENDR) */
    uint32_t db_to_guest;           /* INTID of the SPI the host pends to wake the guest (0 = poll) */
    uint32_t db_to_host;            /* INTID of the SPI the guest pends to wake the host (0 = poll) */
    uint32_t host_cpuif;            /* GIC CPU-interface bits of the host's cores */
    uint32_t screen_on;             /* 1 / 0 */
    uint32_t surf_w, surf_h, surf_stride, surf_format;
    uint32_t surf_nbuf;             /* 2 */
    uint32_t surf_off[2];           /* from SHM_BASE */
    uint32_t oops_off, oops_size;   /* the guest's ramoops console zone */
    uint32_t pad0[5];
    /* ---- written by the guest ---- */
    uint32_t guest_state;
    uint32_t guest_heartbeat;       /* guestd, every frame / 100 ms */
    uint64_t guest_uptime_ms;
    uint32_t guest_ncpus;           /* online CPUs */
    uint32_t guest_cpuif;           /* the guest CPUs' GIC CPU-interface bits, ORed (sbguest.ko) */
    uint32_t surf_front;            /* buffer index the host shows */
    uint32_t surf_seq;              /* +1 per published frame (after surf_front) */
    uint32_t damage_x, damage_y, damage_w, damage_h;   /* of the last frame; w = 0: whole surface */
    uint32_t touches_seen;          /* touch messages consumed */
    uint32_t guest_err;
    char guest_kver[64];            /* uname -r */
    char title[32];                 /* the app's title for Starbite's frame */
    uint32_t pad1[16];
};

/* ---- rings ----------------------------------------------------------------------------------------- */
enum {
    /* host -> guest */
    SBG_MSG_TOUCH = 1,      /* a = slot, b = SBG_TOUCH_*, x / y in surface pixels */
    SBG_MSG_KEY = 2,        /* a = Linux key code, b = 1 down / 0 up */
    SBG_MSG_SCREEN = 3,     /* a = 1 on / 0 off */
    SBG_MSG_STOP = 4,       /* power the cluster off (a = reason) */
    SBG_MSG_FOCUS = 5,      /* a = 1: the surface is shown full screen / 0: in the background */
    /* guest -> host */
    SBG_MSG_HELLO = 64,     /* a = ncpus, b = surface ready */
    SBG_MSG_BYE = 65,       /* user space done; the kernel powers the CPUs off next */
    SBG_MSG_TITLE = 66,     /* hdr.title changed */
};
enum { SBG_TOUCH_DOWN = 0, SBG_TOUCH_MOVE = 1, SBG_TOUCH_UP = 2 };

struct sbshm_msg {
    uint16_t type;
    uint16_t flags;
    uint32_t a, b, c;
    int32_t x, y;
    uint32_t t_ms;
    uint32_t pad;
};                                  /* 32 bytes */

#define SBSHM_RING_N 128U           /* power of two */
struct sbshm_ring {
    volatile uint32_t head;         /* producer: next slot to write */
    uint32_t pad_h[15];
    volatile uint32_t tail;         /* consumer: next slot to read */
    uint32_t pad_t[15];
    struct sbshm_msg m[SBSHM_RING_N];
};                                  /* 128 + 4096 bytes: fits the 8 KiB slots above */

static inline void sbshm_dmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/* 1 = queued, 0 = full. Volatile, word-wise copies: the region is non-cacheable (or device) memory. */
static inline int sbshm_put(struct sbshm_ring *r, const struct sbshm_msg *msg)
{
    uint32_t h = r->head, t = r->tail;
    if (h - t >= SBSHM_RING_N)
        return 0;
    volatile uint32_t *d = (volatile uint32_t *)&r->m[h & (SBSHM_RING_N - 1)];
    const uint32_t *s = (const uint32_t *)msg;
    for (unsigned i = 0; i < sizeof(*msg) / 4; ++i)
        d[i] = s[i];
    sbshm_dmb();
    r->head = h + 1;
    return 1;
}

/* 1 = one message read into *msg, 0 = empty */
static inline int sbshm_get(struct sbshm_ring *r, struct sbshm_msg *msg)
{
    uint32_t t = r->tail, h = r->head;
    if (t == h)
        return 0;
    sbshm_dmb();
    const volatile uint32_t *s = (const volatile uint32_t *)&r->m[t & (SBSHM_RING_N - 1)];
    uint32_t *d = (uint32_t *)msg;
    for (unsigned i = 0; i < sizeof(*msg) / 4; ++i)
        d[i] = s[i];
    sbshm_dmb();
    r->tail = t + 1;
    return 1;
}

/* ---- pstore/ramoops console zone (fs/pstore/ram_core.c, ECC off) ----------------------------------- */
#define SBSHM_PRZ_SIG 0x43474244U   /* "DBGC" */
struct sbshm_prz {
    uint32_t sig;
    uint32_t start;                 /* next write offset in data[] */
    uint32_t size;                  /* bytes filled (<= capacity) */
    uint8_t data[];
};

/* ---- the M3 entry trampoline ------------------------------------------------------------------------
 * PSCI CPU_ON enters at `entry` with the MMU off. On the S9 the entry must be BELOW 4 GiB: P1 (2026-11-08)
 * CPU_ON'd an M3 at 0x8_8008_0000, PSCI said 0, the cluster powered up and the core never ran (the XNU port
 * hit the same: NOTES.md 2026-10-26, "the <4 GiB CPU_ON trampoline"). So the host puts these words in a page
 * of its own below 4 GiB (Starbite OS: its non-cacheable window at 0xa010_0000; QEMU: the shared header page)
 * and passes them as the entry. x0 should be the context id, but the trampoline loads the DTB address
 * itself; the arm64 boot protocol wants x0 = DTB, x1..x3 = 0, MMU off. The first instruction leaves a
 * breadcrumb (its own address, at word 12) so the host can tell "never ran" from "ran and stalled later".
 * The host patches the Image address into words 8/9 and the DTB address into 10/11. */
#define SBG_TRAMP_IMG 8
#define SBG_TRAMP_DTB 10
#define SBG_TRAMP_CRUMB 12
#define SBG_TRAMP_WORDS 14
static const uint32_t sbg_tramp[SBG_TRAMP_WORDS] = {
    0x10000185U,                    /* adr x5, #48  (word 12) */
    0xf90000a5U,                    /* str x5, [x5]  the breadcrumb */
    0x58000100U,                    /* ldr x0, #32  (the DTB address at word 10) */
    0xaa1f03e1U,                    /* mov x1, xzr */
    0xaa1f03e2U,                    /* mov x2, xzr */
    0xaa1f03e3U,                    /* mov x3, xzr */
    0x58000044U,                    /* ldr x4, #8   (the Image address at word 8) */
    0xd61f0080U,                    /* br  x4 */
    0, 0,                           /* Image physical address (low, high) */
    0, 0,                           /* DTB physical address (low, high) */
    0, 0,                           /* breadcrumb */
};

#endif
