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
};

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
};

enum { SBG_OFF = 0, SBG_LOADED, SBG_BOOTING, SBG_RUNNING, SBG_STOPPING, SBG_FAILED };

struct sbg_stats {
    uint32_t state, boots, hello, frames, touches_sent, msgs_in, lines_lost, lines_repaired;
    uint32_t doorbells_out, guest_state, guest_ncpus, guest_cpuif;
    uint32_t hello_ncpus;                           /* CPUs online at HELLO (guest_ncpus: at the last update) */
    uint64_t t_cpu_on, t_hello, t_first_frame, t_stop, t_off;  /* ms since sbg_start (t_off: since sbg_stop) */
    const char *why;                                /* SBG_FAILED: the reason */
};

void sbg_init(const struct sbg_os *os, const struct sbg_cfg *cfg, uint8_t host_cpuif);
/* GIC setup for the doorbells (edge; the host's to the host CPU interface, enabled) and a snapshot of the
 * host's own lines; call after the host's own GIC reset and before sbg_start. */
void sbg_gic_prepare(void);
/* Copy the guest in (word copies) and write the header. 0, or < 0 (and stats.why). */
int sbg_load(const void *image, uint64_t image_size, const void *dtb, uint64_t dtb_size, const void *initrd,
             uint64_t initrd_size);
int sbg_start(void);
/* From the host's tick: the guest's messages and log, state changes, timeouts, the line checks. */
void sbg_poll(void);
/* The host received db_host (from its own interrupt loop). */
void sbg_doorbell(void);
/* A new frame since *seq: 1 and the front buffer, else 0. */
int sbg_frame(uint32_t *seq, const volatile uint32_t **pixels, uint32_t *stride_bytes);
int sbg_touch(uint32_t slot, uint32_t phase, int32_t x, int32_t y);
int sbg_key(uint32_t code, int down);
int sbg_screen(int on);
/* Any host -> guest message (SBG_MSG_*), with the doorbell. 1 queued, 0 not (not running, or the ring full). */
int sbg_send(uint16_t type, uint32_t a, uint32_t b);
int sbg_stop(void);
const struct sbg_stats *sbg_stats(void);
volatile struct sbshm_hdr *sbg_hdr(void);

#endif
