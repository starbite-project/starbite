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
#define SBG_S9_TRAMP        0x0a0100000ULL      /* below 4 GiB: Starbite's non-cacheable window (P1b) */

/* QEMU virt (gic-version=2, -m 2048: RAM 0x4000_0000..0xBFFF_FFFF); the "store" (what Starbite OS reads
 * from UFS on the phone) is put in RAM by -device loader at SBG_QEMU_STORE. */
#define SBG_QEMU_LINUX_BASE 0x080000000ULL
#define SBG_QEMU_LINUX_SIZE 0x03c000000ULL      /* 960 MiB */
#define SBG_QEMU_SHM_BASE   0x0bc000000ULL
#define SBG_QEMU_SHM_SIZE   0x004000000ULL
#define SBG_QEMU_GICD       0x08000000ULL
#define SBG_QEMU_GICC       0x08010000ULL
#define SBG_QEMU_STORE      0x048000000ULL      /* manifest page, then Image, DTB, initrd */
#define SBG_QEMU_TRAMP      0x047000000ULL      /* sbsim's page for the trampolines (outside the guest's RAM) */
/* in the trampoline page: +0 the host's boot trampoline (sbg_tramp), +SBG_TRAMP_GUEST_OFF the slot the guest
 * kernel writes for its own CPU_ONs (linux/0002 patch, DT /psci "starbite,cpu-on-trampoline") */
#define SBG_TRAMP_GUEST_OFF 0x40U

/* inside the guest's RAM */
#define SBG_DTB_OFF         0x08000000ULL       /* +128 MiB: past any Image + BSS */
#define SBG_RD_OFF          0x08200000ULL

/* ---- inside the shared region ---------------------------------------------------------------------- */
#define SBSHM_HDR_OFF       0x000000U           /* struct sbshm_hdr (4 KiB page) */
#define SBSHM_TRAMP_OFF     0x000f00U           /* the M3 entry trampoline (sbg_tramp, 64 bytes) */
#define SBSHM_H2G_OFF       0x001000U           /* ring: host -> guest (control + input, in order) */
#define SBSHM_G2H_OFF       0x003000U           /* ring: guest -> host */
#define SBSHM_ROWS_OFF      0x008000U           /* uint32_t row_seq[SBSHM_ROWS_MAX]: damage per surface row */
#define SBSHM_ROWS_MAX      4096U
#define SBSHM_OOPS_OFF      0x100000U           /* pstore/ramoops console zone (the guest's kernel log) */
#define SBSHM_OOPS_SIZE     0x080000U           /* 512 KiB */
#define SBSHM_SURF_OFF      0x200000U           /* surface buffers, each surf_stride * surf_h */
/* contract v2 (docs/STARBITE-LINUX-APPS.md L3-L7; sections "v2" below). The surfaces end below 16 MiB (two buffers
 * of at most 720 x 1440 x 4 = 4 MiB each from 2 MiB). */
#define SBSHM_CAT_OFF       0x1000000U          /* struct sbshm_cat: the apps the guest can show (guest writes) */
#define SBSHM_STORE_OFF     0x1020000U          /* struct sbshm_store: the App Store's packages (guest writes) */
#define SBSHM_STORE3_OFF    0x1030000U          /* v3: struct sbshm_store3 (Track F, starbite/appstore/sbshm-store3.h):
                                                 * per-package versions / sources / progress, parallel to store.pkg[] */
#define SBSHM_ICON_OFF      0x1040000U          /* the icon pool: SBSHM_ICONS_MAX x 160 x 160 ARGB (guest writes) */
#define SBSHM_BLK_OFF       0x2000000U          /* struct sbshm_blk: devices + request / completion rings */
#define SBSHM_BLKDATA_OFF   0x2010000U          /* the block data window: SBSHM_BLK_SLOTS x SBSHM_BLK_SLOT bytes */
#define SBSHM_NET_OFF       0x2800000U          /* v3: sbnet, the IP packet rings (sbshm-net.h), 2 MiB */
#define SBSHM_P9_OFF        0x2A00000U          /* v3: sbp9, the shared files' 9P2000.L channel (sbshm-p9.h), 2 MiB */
#define SBSHM_SND_OFF       0x2C00000U          /* v3: sbsnd, PCM playback / capture (sbshm-snd.h), 1 MiB */
#define SBSHM_CLIP_OFF      0x2D00000U          /* v3: struct sbshm_clip, the clipboard both ways, 256 KiB */
#define SBSHM_NOTIF_OFF     0x2D40000U          /* v3: struct sbshm_notifs, notifications (guest writes), 64 KiB */
#define SBSHM_V3_FREE_OFF   0x2D50000U          /* .. 0x4000000: free (reserved for zero-copy app buffers) */

#define SBSHM_MAGIC         0x31764d4853425353ULL   /* "SSBSHMv1" */
#define SBSHM_VERSION       1U
#define SBSHM_VERSION_V2    2U                  /* hdr.version = 2: the host serves the v2 regions (blk devices) */
#define SBSHM_VERSION_V3    3U                  /* hdr.version = 3: v2 + hdr.host_services says which v3 services */

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
    /* v3 (were pad0[0..2]: zero from a v1/v2 host) */
    uint32_t host_unix_s;           /* wall clock, seconds since 1970 UTC, updated every tick; 0 = unknown */
    int32_t host_tz_min;            /* the user's UTC offset in minutes (local = UTC + this) */
    uint32_t host_services;         /* SBSHM_SVC_*: which v3 services the host serves in this boot */
    uint32_t host_surf_reading;     /* v3: the surface buffer (0 / 1) the host's GPU samples right now, SBSHM_NONE =
                                     * none: the guest never renders into that buffer (it waits, or skips the frame).
                                     * Zero-copy display: the GPU reads the front buffer in place (no CPU copy), so
                                     * pixels must be opaque (X byte = 0xff) and written through a non-cacheable
                                     * mapping (/dev/sbshm is write-combine = Normal non-cacheable). Honour it only
                                     * when hdr.version >= 3 (a v2 host leaves 0 here). */
    uint32_t pad0[1];
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
    uint32_t rows_valid;            /* 1: the guest keeps row_seq (SBSHM_ROWS_OFF); 0: copy the whole surface */
    uint32_t pad1[15];
};

/* ---- damage per row (guestd since 2026-11-09) ----------------------------------------------------------
 * row_seq[y] = the surf_seq of the frame in which surface row y last changed. The guest writes the rows of a
 * new frame into the back buffer, sets row_seq[y] = the new seq for every row that differs from the current front,
 * then publishes (surf_front, dmb, surf_seq) as before; it keeps both buffers whole (each has every row of its
 * frame). A host that drew the frame `drawn` copies only the rows with (int32_t)(row_seq[y] - drawn) > 0 from the
 * front buffer, then drawn = the surf_seq it read before the rows (a frame published meanwhile has newer rows and
 * is copied next time). A host that ignores the table copies everything, as before. */
static inline int sbshm_row_newer(uint32_t row_seq, uint32_t drawn)
{
    return (int32_t)(row_seq - drawn) > 0;
}

/* ---- rings ----------------------------------------------------------------------------------------- */
enum {
    /* host -> guest */
    SBG_MSG_TOUCH = 1,      /* a = slot, b = SBG_TOUCH_*, x / y in surface pixels */
    SBG_MSG_KEY = 2,        /* a = Linux key code, b = 1 down / 0 up */
    SBG_MSG_SCREEN = 3,     /* a = 1 on / 0 off */
    SBG_MSG_STOP = 4,       /* power the cluster off (a = reason) */
    SBG_MSG_FOCUS = 5,      /* a = 1: the surface is shown full screen / 0: in the background */
    SBG_MSG_LOAD = 6,       /* test load: a = threads spinning for b seconds (P2d: the M3s under load) */
    /* host -> guest, v2 */
    SBG_MSG_VIEWPORT = 7,   /* a = the rows of the surface the host shows (the keyboard up: fewer), b = the
                             * width: the guest configures the active window to b x a */
    SBG_MSG_LAUNCH = 8,     /* a = catalog index, b = the catalog's layout_seq it was read at: start the app (or
                             * raise it); a stale layout_seq is ignored */
    SBG_MSG_ACTIVATE = 9,   /* a = catalog index whose window goes into the surface; SBSHM_NONE = none (home) */
    SBG_MSG_APP_CLOSE = 10, /* a = catalog index: close its windows (then SIGTERM, SIGKILL after 3 s) */
    SBG_MSG_TEXT = 11,      /* commit text to the focused text-input-v3 field: c bytes (1..8) of UTF-8 in a, b
                             * (little-endian, a first); a character never splits across two messages */
    SBG_MSG_PKG = 12,       /* a = store index, b = SBG_PKG_OP_*, c = the store seq it was read at */
    SBG_MSG_BLK_DONE = 13,  /* (reserved: sbblk uses the doorbells alone, see "sbblk") */
    /* host -> guest, v3 */
    SBG_MSG_CLIP = 14,      /* the host's clipboard changed: a = clip.h2g.seq (sbwl offers it to Wayland apps) */
    SBG_MSG_NOTIF_ACT = 15, /* a = notification id, b = action index (0 = the default action, the tap) or
                             * SBSHM_NONE = dismissed by the user */
    SBG_MSG_KEYMOD = 16,    /* a = SBSHM_MOD_* held for the next keys / text (Ctrl, Alt, Super from the shell's key
                             * bar); 0 = none. Keys themselves stay SBG_MSG_KEY */
    /* guest -> host */
    SBG_MSG_HELLO = 64,     /* a = ncpus, b = surface ready */
    SBG_MSG_BYE = 65,       /* user space done; the kernel powers the CPUs off next */
    SBG_MSG_TITLE = 66,     /* hdr.title changed */
    /* guest -> host, v2 */
    SBG_MSG_CATALOG = 67,   /* the catalog (apps, icons) changed: a = its new seq */
    SBG_MSG_WINDOW = 68,    /* a = catalog index whose window is in the surface now (SBSHM_NONE: none), b = 1 the
                             * app has a window at all / 0 it has none (closed, or still starting) */
    SBG_MSG_TEXT_FOCUS = 69,/* a = 1 a text field wants the keyboard / 0 not; b = text-input-v3 content purpose */
    SBG_MSG_PKG_STATUS = 70,/* a = store index, b = SBG_PKG_*, c = percent: also in the store entry */
    SBG_MSG_BLK = 71,       /* (reserved: sbblk uses the doorbells alone) */
    SBG_MSG_STORE = 72,     /* the store index changed: a = its new seq */
    /* guest -> host, v3 */
    SBG_MSG_CLIP_SET = 73,  /* a Wayland app copied: a = clip.g2h.seq */
    SBG_MSG_NOTIFY = 74,    /* a = notification slot, b = its id: post (or replace, same id) */
    SBG_MSG_NOTIF_CLOSE = 75,/* a = id: the app withdrew it */
};
#define SBSHM_NONE 0xffffffffU
/* v2, who uses the rings in the guest: the rings are single-producer single-consumer, so ONE guest process owns
 * them: sbwl (the compositor). Other guest daemons talk to sbwl over the unix socket /run/sbwl.sock, one text line
 * per message: sbpkg -> sbwl "store SEQ" (the store table changed), "pkg IDX STATE PCT", "rescan" (the .desktop
 * files changed); sbwl -> sbpkg "op IDX OP SEQ" (an SBG_MSG_PKG from the host). The kernel (sbguest.ko: sbblk)
 * never uses the message rings. */
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

/* ---- typing: ASCII to Linux key codes (US layout) --------------------------------------------------------
 * For a host's keyboard (Starbite OS's on-screen keys, sbsim, the phone payloads): SBG_MSG_KEY with the code, down
 * then up, and KEY_LEFTSHIFT (42) around it when *shift. 0 for a character with no key. */
static inline unsigned sbshm_ascii_key(char ch, int *shift)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const unsigned char letter[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                             49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    static const char plain[] = "1234567890-=[];'`\\,./ \n\t\b";
    static const unsigned char plain_k[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 26, 27, 39, 40, 41, 43, 51, 52, 53,
                                            57, 28, 15, 14};
    static const char shifted[] = "!@#$%^&*()_+{}:\"~|<>?";
    static const unsigned char shifted_k[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 26, 27, 39, 40, 41, 43, 51, 52, 53};
    *shift = 0;
    for (unsigned i = 0; i < 26; ++i) {
        if (ch == lower[i])
            return letter[i];
        if (ch == lower[i] - 32) {
            *shift = 1;
            return letter[i];
        }
    }
    for (unsigned i = 0; plain[i]; ++i)
        if (ch == plain[i])
            return plain_k[i];
    for (unsigned i = 0; shifted[i]; ++i)
        if (ch == shifted[i]) {
            *shift = 1;
            return shifted_k[i];
        }
    return 0;
}
#define SBSHM_KEY_LEFTSHIFT 42U

/* ==== contract v2 (docs/STARBITE-LINUX-APPS.md L3-L7) ================================================== */

/* ---- v2: seqlocked tables (catalog, store) ------------------------------------------------------------
 * One writer (the guest). It sets seq odd, dmb, writes the table (and the icons it points at), dmb, seq + 1 (even).
 * A reader reads seq (even, else retry later), dmb, copies what it needs, dmb, reads seq again: equal = the copy is
 * whole. Indices are valid for one seq: messages that carry an index also carry the seq it was read at, and the
 * guest ignores a stale one. */
static inline uint32_t sbshm_seq_begin(const volatile uint32_t *seq)
{
    uint32_t s = *seq;
    sbshm_dmb();
    return s;
}
static inline int sbshm_seq_ok(const volatile uint32_t *seq, uint32_t begun)
{
    sbshm_dmb();
    return !(begun & 1U) && *seq == begun;
}

/* ---- v2: the icon pool (catalog apps and store packages share it) ---------------------------------------
 * Icon i at SBSHM_ICON_OFF + i * SBSHM_ICON_BYTES: SBSHM_ICON_PX x SBSHM_ICON_PX, premultiplied ARGB words
 * (0xAARRGGBB, sbtk's tk_bmp format), rows packed. The shell scales it to its 62 pt (229 px) icon. */
#define SBSHM_ICON_PX       160U
#define SBSHM_ICON_BYTES    (SBSHM_ICON_PX * SBSHM_ICON_PX * 4U)
#define SBSHM_ICONS_MAX     160U                /* 15.6 MiB, to SBSHM_BLK_OFF */
#define SBSHM_ICONS_CAT     64U                 /* 0..63: the catalog's (written by sbwl); 64..159: the store's
                                                 * (written by sbpkg, under the store seq) */

/* ---- v2: the catalog: what the home screen shows for Linux --------------------------------------------- */
#define SBSHM_APPS_MAX      96U
enum {
    SBSHM_APP_RUNNING = 1U << 0,    /* a process of it runs */
    SBSHM_APP_WINDOW = 1U << 1,     /* it has a mapped toplevel */
    SBSHM_APP_TERMINAL = 1U << 2,   /* Terminal=true in its .desktop: run inside foot */
    SBSHM_APP_KEYBOARD = 1U << 3,   /* keep the keyboard up (terminals: no text-input-v3) */
    SBSHM_APP_SYSTEM = 1U << 4,     /* in the base image (cannot be removed) */
};
struct sbshm_app {
    char id[40];                    /* the desktop id ("org.gnome.Calculator"), NUL-terminated */
    char name[32];                  /* Name= (the home's label), NUL-terminated UTF-8 */
    uint32_t flags;                 /* SBSHM_APP_* */
    uint32_t icon;                  /* pool index, or SBSHM_NONE */
    uint32_t pkg;                   /* store index of the package that brought it, or SBSHM_NONE */
    uint32_t rss_kb;                /* resident memory of its processes (0 while not running) */
    uint32_t pad[2];
};                                  /* 96 bytes */
struct sbshm_cat {
    volatile uint32_t seq;
    uint32_t n;                     /* apps[0..n) */
    uint32_t active;                /* the index whose window is in the surface, or SBSHM_NONE */
    uint32_t mem_total_kb, mem_avail_kb;   /* the guest's /proc/meminfo (MemTotal, MemAvailable) */
    uint32_t swap_total_kb, swap_free_kb;  /* zram */
    uint32_t layout_seq;            /* changes only when the apps' ids or order change (seq also moves with rss
                                     * and flags every ~2 s): SBG_MSG_LAUNCH carries this one */
    uint32_t pad[8];
    struct sbshm_app app[SBSHM_APPS_MAX];
};

/* ---- v2: the App Store's packages (a curated index from the repo device, see L6) ------------------------ */
#define SBSHM_PKGS_MAX      160U
enum {                              /* sbshm_pkg.state */
    SBG_PKG_IDLE = 0, SBG_PKG_QUEUED, SBG_PKG_INSTALLING, SBG_PKG_REMOVING, SBG_PKG_FAILED,
};
enum { SBG_PKG_OP_INSTALL = 1, SBG_PKG_OP_REMOVE = 2, SBG_PKG_OP_CANCEL = 3 };
enum {                              /* sbshm_pkg.flags */
    SBSHM_PKG_INSTALLED = 1U << 0,
    SBSHM_PKG_SYSTEM = 1U << 1,     /* part of the base image */
    SBSHM_PKG_TOO_BIG = 1U << 2,    /* ram_mb above what the guest can give it: install allowed, with a warning */
    SBSHM_PKG_NO_SPACE = 1U << 3,   /* inst_kb above the overlay's free space: install refused */
    SBSHM_PKG_FEATURED = 1U << 4,
};
struct sbshm_pkg {
    char pkg[40];                   /* the Debian package (or a meta name), NUL-terminated */
    char name[32];                  /* the store's title */
    char summary[96];               /* one line */
    char category[16];              /* "Internet", "Office", "Graphics", "Utilities", "Games", "Development" */
    char err[32];                   /* SBG_PKG_FAILED: why, in words */
    uint32_t dl_kb;                 /* to read from the repo (all missing dependencies) */
    uint32_t inst_kb;               /* the overlay space it takes */
    uint32_t ram_mb;                /* honest typical resident memory while in use */
    uint32_t flags;                 /* SBSHM_PKG_* */
    uint32_t state;                 /* SBG_PKG_* */
    uint32_t pct;                   /* progress 0..100 of the current operation */
    uint32_t icon;                  /* pool index, or SBSHM_NONE */
    uint32_t pad;
};                                  /* 248 bytes */
struct sbshm_store {
    volatile uint32_t seq;
    uint32_t n;
    uint32_t overlay_total_kb, overlay_free_kb;
    uint32_t repo_ok;               /* 1: the repo device is there and its index read */
    uint32_t pad[11];
    struct sbshm_pkg pkg[SBSHM_PKGS_MAX];
};

/* ---- v2: sbblk: block devices served by the host ----------------------------------------------------------
 * The host fills dev[] before CPU_ON (hdr.version = 2). The guest's kernel (sbguest.ko) submits requests to the
 * request ring (it is the producer: req_head; the host advances req_tail) and pends the host's doorbell (db_to_host;
 * no message: the rings belong to sbwl and blocks are needed before sbwl runs, for the root). The host serves them in
 * order at every poll, writes each completion (same id, status) to the completion ring and pends the guest's doorbell
 * (db_to_guest); the guest's doorbell handler reaps completions. Data lives in the data window:
 * a request's buffer is slot data_slot (SBSHM_BLK_SLOT bytes each), so count <= SBSHM_BLK_SLOT / 512. Reads: the
 * host fills the slot before the completion. Writes: the guest fills it before the request. Flush: everything
 * completed before it is on the host's storage when it completes. Sector = 512 bytes, but the guest uses 4096-byte
 * logical blocks: every request starts at a multiple of 8 sectors and is a multiple of 8 sectors long (the phone's
 * storage is 4 KiB blocks; a host may refuse anything else with -EINVAL). Overlay file systems: mkfs with -b 4096. */
#define SBSHM_BLK_DEVS      4U
#define SBSHM_BLK_RING      64U             /* power of two */
#define SBSHM_BLK_SLOT      0x10000U        /* 64 KiB */
#define SBSHM_BLK_SLOTS     64U             /* the data window: 4 MiB */
enum { SBSHM_BLK_BASE = 0, SBSHM_BLK_OVERLAY = 1, SBSHM_BLK_REPO = 2 };   /* conventional device numbers */
enum { SBSHM_BLKDEV_PRESENT = 1U << 0, SBSHM_BLKDEV_RO = 1U << 1 };
enum { SBG_BLK_READ = 1, SBG_BLK_WRITE = 2, SBG_BLK_FLUSH = 3 };
struct sbshm_blkdev {
    uint64_t sectors;
    uint32_t flags;                 /* SBSHM_BLKDEV_* */
    char name[20];                  /* "base", "overlay", "repo": what the host serves it from, for logs */
};                                  /* 32 bytes */
struct sbshm_blkreq {
    uint32_t id;                    /* the guest's tag, echoed in the completion */
    uint16_t op;                    /* SBG_BLK_* */
    uint16_t dev;
    uint64_t sector;
    uint32_t count;                 /* sectors */
    uint32_t data_slot;
    int32_t status;                 /* completion: 0 ok, < 0 a Linux errno (-EIO, -EROFS, -ENOSPC) */
    uint32_t pad;
};                                  /* 32 bytes */
struct sbshm_blk {
    struct sbshm_blkdev dev[SBSHM_BLK_DEVS];
    uint32_t pad0[32];
    volatile uint32_t req_head;     /* guest */
    uint32_t pad1[15];
    volatile uint32_t req_tail;     /* host */
    uint32_t pad2[15];
    volatile uint32_t done_head;    /* host */
    uint32_t pad3[15];
    volatile uint32_t done_tail;    /* guest */
    uint32_t pad4[15];
    struct sbshm_blkreq req[SBSHM_BLK_RING];
    struct sbshm_blkreq done[SBSHM_BLK_RING];
};                                  /* 4 KiB + 0.5 KiB: fits before SBSHM_BLKDATA_OFF */

/* ==== contract v3 (Track E, 2026-11-12: "apps work fully") ===========================================
 * hdr.version = 3 and hdr.host_services name what the host serves; the guest uses a service only when its bit is
 * set (a v2 host leaves host_services 0). Kernel services (net, 9p, snd) use the doorbells alone, like sbblk; the
 * desktop ones (clip, notif) go through sbwl and the message rings. */
enum {
    SBSHM_SVC_NET = 1U << 0,        /* sbnet (sbshm-net.h): the IP rings at SBSHM_NET_OFF */
    SBSHM_SVC_P9 = 1U << 1,         /* sbp9 (sbshm-p9.h): Starbite's files at /home/user/Starbite */
    SBSHM_SVC_SND = 1U << 2,        /* sbsnd (sbshm-snd.h): speaker / mic */
    SBSHM_SVC_CLIP = 1U << 3,       /* struct sbshm_clip */
    SBSHM_SVC_NOTIF = 1U << 4,      /* struct sbshm_notifs */
    SBSHM_SVC_CLOCK = 1U << 5,      /* hdr.host_unix_s is real */
};
enum { SBSHM_MOD_CTRL = 1U << 0, SBSHM_MOD_ALT = 1U << 1, SBSHM_MOD_SUPER = 1U << 2, SBSHM_MOD_SHIFT = 1U << 3 };

/* ---- v3: the clipboard. Two seqlocked slots, one writer each: g2h (sbwl, when a Wayland app sets the selection;
 * then SBG_MSG_CLIP_SET) and h2g (the host, when Starbite copies; then SBG_MSG_CLIP). Text is UTF-8 without a NUL;
 * images are PNG. Larger content is not offered (len 0, mime names what it was). */
#define SBSHM_CLIP_MAX      (0x20000U - 64U)
struct sbshm_clip_slot {
    volatile uint32_t seq;
    uint32_t len;                   /* bytes in data */
    char mime[40];                  /* "text/plain;charset=utf-8" or "image/png" */
    uint32_t pad[4];
    uint8_t data[SBSHM_CLIP_MAX];
};                                  /* 128 KiB */
struct sbshm_clip {
    struct sbshm_clip_slot g2h;     /* guest writes */
    struct sbshm_clip_slot h2g;     /* host writes */
};

/* ---- v3: notifications (org.freedesktop.Notifications in the guest -> Starbite's Notification Centre).
 * sbwl writes slot (id % SBSHM_NOTIF_SLOTS) under the table's seqlock, then SBG_MSG_NOTIFY (a = slot, b = id). The
 * host copies it at once (a slot may be reused by a later id). A tap: SBG_MSG_NOTIF_ACT (b = 0, the default action;
 * the guest also raises the app); a swipe away: b = SBSHM_NONE. */
#define SBSHM_NOTIF_SLOTS   32U
#define SBSHM_NOTIF_ACTIONS 3U
enum { SBSHM_NOTIF_LOW = 0, SBSHM_NOTIF_NORMAL = 1, SBSHM_NOTIF_CRITICAL = 2 };
struct sbshm_notif {
    uint32_t id;                    /* the guest's notification id (never 0) */
    uint32_t app;                   /* catalog index of the app that sent it, or SBSHM_NONE */
    uint32_t urgency;               /* SBSHM_NOTIF_* */
    uint32_t timeout_ms;            /* 0 = until dismissed */
    char app_name[32];              /* NUL-terminated UTF-8 */
    char summary[96];
    char body[256];                 /* plain text (markup stripped) */
    char action[SBSHM_NOTIF_ACTIONS][24];  /* button labels for actions 1..3 ("" = none); action 0 = the tap */
    uint32_t icon;                  /* icon pool index (the app's), or SBSHM_NONE */
    uint32_t pad[9];
};                                  /* 512 bytes */
struct sbshm_notifs {
    volatile uint32_t seq;
    uint32_t pad[15];
    struct sbshm_notif n[SBSHM_NOTIF_SLOTS];
};

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
