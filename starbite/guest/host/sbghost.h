/* SPDX-License-Identifier: GPL-2.0-only
 * sbghost: Starbite OS's side of the guest Linux on the M3 cluster (docs/STARBITE-OS-LINUX-M3.md), OS
 * independent like the Wi-Fi core: the host OS gives a shim (struct sbg_os) and calls sbg_poll() from its timer
 * tick; nothing here blocks (start + poll). Used by sbsim (QEMU), the phone probe (libsb) and, later, Starbite
 * OS's kernel (kern/guest.c: one more service next to Wi-Fi and the modem).
 *
 * Life cycle (hdr.host_state): OFF -> sbg_load (Image, DTB, initrd copied into the carve-out; the shared region
 * and the header written) -> sbg_start (the M3 PMU defaults, PSCI CPU_ON of the first M3 core through the
 * trampoline) -> RUNNING (the guest said HELLO; frames, input, the kernel log) -> sbg_stop (MSG_STOP) -> OFF
 * once every guest core is off. There is no way to force an M3 core off from an A55 (PSCI has no "CPU_OFF that
 * core"): a guest that never powers off keeps the cluster on until the next reset; sbg_poll reports it.
 * Memory: every access to the shared region and the carve-out is a volatile word access (the host maps them
 * non-cacheable; sbsim runs with the MMU off).
 */
#ifndef SBGHOST_H
#define SBGHOST_H

#include <stdint.h>
#include "../sbshm.h"

struct sbg_os {
    uint64_t (*ms)(void);
    void (*log)(const char *line);                  /* sbghost's own lines ("guest: ...") */
    void (*guest_log)(const char *line);            /* one line of the guest kernel's log (ramoops console) */
    int64_t (*cpu_on)(uint64_t mpidr, uint64_t entry, uint64_t ctx);   /* PSCI CPU_ON, the 32-bit answer */
    int (*cluster_off)(void);                       /* 1 every guest core off, 0 not, < 0 unknown */
    int (*m3_prep)(void);                           /* the vendor PMU defaults before CPU_ON (NULL: none); 1 ok */
    /* sbblk (contract v2; optional: NULL = no block device is offered). One request of the guest: op SBG_BLK_READ /
     * SBG_BLK_WRITE of `count` sectors (512 bytes; count <= SBSHM_BLK_SLOT / 512) at `sector` of device `dev`
     * (cfg.blk[dev]), to / from buf (the request's slot in the data window: non-cacheable shared memory), or
     * SBG_BLK_FLUSH (count 0, buf NULL: everything written before is on the storage when it returns). sbghost has
     * checked the device (PRESENT, a write only when not RO) and the range. 0, or a negative Linux errno (SBG_E*).
     * Called from sbg_poll: it should take about a millisecond (see cfg.blk_per_poll / blk_poll_ms). */
    int (*blk_io)(uint32_t dev, uint32_t op, uint64_t sector, uint32_t count, void *buf);
    /* ---- contract v3 (all optional) ---- */
    /* the wall clock: seconds since 1970 UTC (0 = unknown), *tz_min = the user's UTC offset in minutes */
    uint32_t (*unix_s)(int32_t *tz_min);
    /* sbnet (sbshm-net.h): one IP packet the guest sent (called from sbg_poll; pkt is a private copy). The host's NAT
     * (Track G) answers with sbg_net_send(). */
    void (*net_rx)(const uint8_t *pkt, unsigned len);
    /* sbp9 (host/sbg_p9.h): Starbite's files for the guest's 9P2000.L server; NULL = none */
    const struct sbg_fs *fs;
    /* sbsnd (host/sbg_snd.h): the speaker / mic for the guest's PCM streams; NULL = none */
    const struct sbg_snd *snd;
};
struct sbg_fs;
struct sbg_snd;
/* the Linux errno values sbblk's completions use */
enum { SBG_EIO = 5, SBG_ENODEV = 19, SBG_EINVAL = 22, SBG_ENOSPC = 28, SBG_EROFS = 30 };

struct sbg_cfg {
    uint64_t linux_base, linux_size;                /* the carve-out (sbshm.h profile) */
    uint64_t shm_base, shm_size;
    uint64_t gicd;                                  /* GIC distributor, for the doorbells and the line checks */
    uint64_t first_mpidr;                           /* the guest's boot CPU: 0x100 on the S9, 4 on QEMU */
    uint64_t tramp_pa;                              /* the entry trampoline: BELOW 4 GiB on the S9 (sbshm.h), in
                                                     * host memory mapped non-cacheable; 0 = the shared header page */
    uint32_t surf_w, surf_h;                        /* the guest surface (XRGB8888, two buffers) */
    uint32_t db_guest, db_host;                     /* doorbell INTIDs (0: none, the guest polls) */
    uint32_t host_lines[8];                         /* INTIDs the host owns (checked + repaired), 0-terminated */
    uint32_t repair;                                /* 1: re-assert host_lines when a guest GIC init took them */
    uint32_t boot_timeout_ms, stop_timeout_ms;
    /* contract v2 (sbshm.h): 1 = hdr.version 2, the v2 tables cleared at load, the block devices below offered */
    uint32_t v2;
    struct sbshm_blkdev blk[SBSHM_BLK_DEVS];       /* PRESENT ones are served through sbg_os.blk_io (sbblk) */
    /* sbblk's work per sbg_poll, so the host's tick stays short: at most blk_per_poll requests (0: 8, that is
     * 512 KiB), and no new request once blk_poll_ms have passed in this poll (0: 4 ms; at least one is served) */
    uint32_t blk_per_poll, blk_poll_ms;
    /* contract v3: the SBSHM_SVC_* the host offers (hdr.version 3 when non-zero; needs v2). A service whose sbg_os
     * part is missing is left out (NET needs net_rx, P9 fs, SND snd, CLOCK unix_s). */
    uint32_t services;
};
#define SBG_BLK_PER_POLL 8U
#define SBG_BLK_POLL_MS  4U

enum { SBG_OFF = 0, SBG_LOADED, SBG_BOOTING, SBG_RUNNING, SBG_STOPPING, SBG_FAILED };

struct sbg_stats {
    uint32_t state, boots, hello, frames, touches_sent, msgs_in, lines_lost, lines_repaired;
    uint32_t doorbells_out, guest_state, guest_ncpus, guest_cpuif;
    uint32_t hello_ncpus;                           /* CPUs online at HELLO (guest_ncpus: at the last update) */
    uint64_t t_cpu_on, t_hello, t_first_frame, t_stop, t_off;  /* ms since sbg_start (t_off: since sbg_stop) */
    const char *why;                                /* SBG_FAILED: the reason */
    /* sbblk */
    uint32_t blk_reads, blk_writes, blk_flushes, blk_errors;   /* requests completed (errors: status != 0) */
    uint32_t blk_doorbells;                         /* completion batches (one doorbell each) */
    uint32_t blk_batch_max;                         /* the most requests served in one poll */
    uint32_t blk_capped;                            /* polls that stopped at a bound with requests left */
    uint32_t blk_poll_ms_max;                       /* the longest time one poll spent on blocks */
    uint64_t blk_rd_sectors, blk_wr_sectors;
};

void sbg_init(const struct sbg_os *os, const struct sbg_cfg *cfg, uint8_t host_cpuif);
/* GIC setup for the doorbells (edge; the host's to the host CPU interface, enabled) and a snapshot of the
 * host's own lines; call after the host's own GIC reset and before sbg_start. */
void sbg_gic_prepare(void);
/* Copy the guest in (word copies) and write the header. 0, or < 0 (and stats.why). */
int sbg_load(const void *image, uint64_t image_size, const void *dtb, uint64_t dtb_size, const void *initrd,
             uint64_t initrd_size);
/* The same when the host already put the files at their places (Starbite OS: store_load straight into the
 * carve-out): the Image at linux_base + SBG_IMAGE_AT (its text_offset must be that), the DTB at + SBG_DTB_OFF,
 * the initrd at + SBG_RD_OFF. Checks the Image's header, then the shared region and the header as sbg_load. */
#define SBG_IMAGE_AT 0x80000ULL
int sbg_load_placed(uint64_t image_size, uint64_t dtb_size, uint64_t initrd_size);
/* The block devices for the next sbg_load, for a host that learns them after sbg_init (Starbite OS: the card's
 * files, resolved at each guest start): SBSHM_BLK_DEVS entries replace cfg.blk; a PRESENT one also selects
 * contract v2. NULL: none (cfg.v2 stays as sbg_init got it). */
void sbg_set_blk(const struct sbshm_blkdev *blk);
int sbg_start(void);
/* From the host's tick: the guest's messages and log, state changes, timeouts, the line checks. */
void sbg_poll(void);
/* The host received db_host (from its own interrupt loop). */
void sbg_doorbell(void);
/* A new frame since *seq: 1 and the front buffer, else 0. */
int sbg_frame(uint32_t *seq, const volatile uint32_t **pixels, uint32_t *stride_bytes);
/* The guest's row table (sbshm.h "damage per row"): NULL when the guest keeps none (copy every row). A host that
 * drew frame `drawn` copies row y when sbshm_row_newer(rows[y], drawn). */
const volatile uint32_t *sbg_rows(void);
int sbg_touch(uint32_t slot, uint32_t phase, int32_t x, int32_t y);
int sbg_key(uint32_t code, int down);
int sbg_screen(int on);
/* Any host -> guest message (SBG_MSG_*), with the doorbell. 1 queued, 0 not (not running, or the ring full). */
int sbg_send(uint16_t type, uint32_t a, uint32_t b);
int sbg_stop(void);
const struct sbg_stats *sbg_stats(void);
volatile struct sbshm_hdr *sbg_hdr(void);

/* ---- contract v2 (sbshm.h; docs/STARBITE-LINUX-APPS.md L3-L7) ----------------------------------------------
 * The guest's tables are read in place with the seqlock helpers (sbshm_seq_begin / sbshm_seq_ok); the icons are
 * SBSHM_ICON_PX square premultiplied ARGB. sbg_changes() tells the platform's poll what to look at again. */
enum { SBG_CH_CATALOG = 1, SBG_CH_STORE = 2, SBG_CH_WINDOW = 4, SBG_CH_TEXT = 8, SBG_CH_PKG = 16 };
unsigned sbg_changes(void);                         /* the SBG_CH_* bits since the last call (then cleared) */
const volatile struct sbshm_cat *sbg_cat(void);
const volatile struct sbshm_store *sbg_store(void);
const volatile uint32_t *sbg_icon(uint32_t i);      /* NULL for SBSHM_NONE / out of range */
uint32_t sbg_window(int *has_window);               /* the catalog index in the surface (SBSHM_NONE: none) */
int sbg_text_focus(uint32_t *purpose);              /* 1: a text field wants the keyboard */
int sbg_launch(uint32_t idx, uint32_t cat_seq);
int sbg_activate(uint32_t idx);                     /* SBSHM_NONE: no window in the surface (home) */
int sbg_app_close(uint32_t idx);
int sbg_viewport(uint32_t rows, uint32_t width);
int sbg_text(const char *utf8);                     /* commit text (split at characters, 8 bytes a message) */
int sbg_pkg(uint32_t idx, uint32_t op, uint32_t store_seq);

/* ---- contract v3 (sbshm.h "contract v3"; Track E) -------------------------------------------------------------- */
enum { SBG_CH_CLIP = 32, SBG_CH_NOTIF = 64 };      /* more sbg_changes() bits */
/* the guest's clipboard (a Wayland app copied): its mime and up to cap bytes; the length (0: empty / too big) */
unsigned sbg_clip_get(char mime[40], uint8_t *buf, unsigned cap);
/* offer Starbite's clipboard to the guest's apps (text: "text/plain;charset=utf-8"); 1 sent, 0 not */
int sbg_clip_set(const char *mime, const uint8_t *data, unsigned len);
/* the next notification event: 1 = *n posted (or replaced: same id), 2 = the app withdrew id *n->id, 0 = none */
int sbg_notif_next(struct sbshm_notif *n);
int sbg_notif_act(uint32_t id, uint32_t action);    /* 0 = the tap, 1..3 its buttons, SBSHM_NONE = dismissed */
int sbg_keymod(uint32_t mods);                      /* SBSHM_MOD_* held for the next keys (the shell's key bar) */
/* sbnet: queue one IP packet to the guest (1 ok, 0 dropped: no link or the ring is full) and the link config */
int sbg_net_send(const uint8_t *pkt, unsigned len);
void sbg_net_config(int link, int uplink, const uint8_t guest_ip[4], const uint8_t gw_ip[4], const uint8_t dns_ip[4],
                    unsigned prefix);

#endif
