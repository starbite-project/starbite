/* SPDX-License-Identifier: GPL-2.0-only
 * ACPM IPC (the APM firmware's queues in APM SRAM 0x02039000, mailbox 0x14100000) for the Starbite Windows
 * drivers: one implementation, shared by sbthermal (CPU DVFS, the lock owner) and sbmali (BUCK6, GPU DVFS).
 * Protocol of starbite/libsb/acpm.c (port of the loader's and the vendor kernel's queue handling): one
 * command slot written, the front index advanced, INTGR0 rung, the reply matched by sequence number.
 * In Windows two drivers use it: every transaction runs under sbthermal's ACPM lock (docs/WIN11-GPU.md 8.3).
 */
#ifndef SBW_ACPM_H
#define SBW_ACPM_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT32 sbw_u32;
#else
#include <stdint.h>
typedef uint32_t sbw_u32;
#endif

#define SBW_ACPM_SRAM_PA 0x02039000ULL
#define SBW_ACPM_SRAM_SIZE 0x30000U
#define SBW_ACPM_MBOX_PA 0x14100000ULL
#define SBW_ACPM_MBOX_SIZE 0x1000U
#define SBW_ACPM_PMIC_CH 2U
#define SBW_ACPM_DVFS_CH (~0U)            /* the first channel of the AP's table (what Linux and XNU use) */

struct sbw_acpm_io {
    void *ctx;
    sbw_u32 (*sram_rd)(void *ctx, sbw_u32 off);
    void (*sram_wr)(void *ctx, sbw_u32 off, sbw_u32 v);
    void (*mbox_wr)(void *ctx, sbw_u32 off, sbw_u32 v);
    void (*delay_us)(void *ctx, unsigned us);
    void (*barrier)(void *ctx);
};

/* The channel entry for id (SBW_ACPM_DVFS_CH = the first one); 0 when the table looks wrong. */
sbw_u32 sbw_acpm_chan(const struct sbw_acpm_io *io, sbw_u32 id);
/* One transaction: cmd in, the reply out. 1 = answered (within ~100 ms). */
int sbw_acpm_send(const struct sbw_acpm_io *io, sbw_u32 c, sbw_u32 cmd[4]);
/* A DVFS request {domain, kHz, FREQ_REQ}. 1 = answered. The callers check domain and frequency. */
int sbw_acpm_dvfs(const struct sbw_acpm_io *io, sbw_u32 domain, sbw_u32 khz);

#endif
