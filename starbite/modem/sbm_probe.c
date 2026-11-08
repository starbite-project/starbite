/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: stage 2, a read-only look at the CP (docs/MODEM.md section 5). It only reads, through the platform's
 * fault-safe probe32, in this order:
 *   1. PMU (0x14060000, always on): CP_STAT, CP_DEBUG, CENTRAL_SEQ_CP_CONFIGURATION/STATUS,
 *      RESET_SEQUENCER_STATUS. Never CP_CTRL_NS / CP_CTRL_S (secure; EL3 SMC only).
 *   2. Mailbox (0x14170000): MCUCTLR, the interrupt mask/status registers, ISSR0..15.
 *   3. Only with SBM_PROBE_SHM: the CP carve-out, three small areas, stopping at the first fault:
 *      modem_if + 0 (16 bytes), the MEMn table at + 0x2a0 (80 bytes, what Samsung's shm_probe reads), and the
 *      first two words of the IPC region (magic, access). TZASC may refuse these reads: that is the question
 *      this step answers. Reads that all return zero without a fault prove nothing (an empty carve-out and a
 *      TZASC read-as-zero denial look alike): the verdict is then "shm-zero-ambiguous".
 * A breadcrumb line goes to the log before each block, so a read that hangs (the watchdog then resets) shows
 * in pmsg which block it was.
 * No register is written, no SMC is made, nothing outside these addresses is read.
 */
#include "sbm.h"

static int rd(const struct sbm_plat *p, struct sbm_probe *pr, uint64_t pa, uint32_t *v)
{
    *v = 0;
    if (!p->probe32)
        return 0;
    if (p->probe32(p->ctx, pa, v))
        return 1;
    pr->faults++;
    *v = 0;
    return 0;
}

static int rd_bytes(const struct sbm_plat *p, struct sbm_probe *pr, uint64_t pa, uint8_t *out, unsigned n)
{
    for (unsigned i = 0; i < n; i += 4) {
        uint32_t v;
        if (!rd(p, pr, pa + i, &v))
            return 0;
        sbm_put32(out + i, v);
    }
    return 1;
}

static void log_pmu(const struct sbm_plat *p, const struct sbm_probe *pr)
{
    if (pr->pmu_ok)
        sbm_logf(p, "PMU: RESET_SEQUENCER_STATUS 0x%08x (seq %u, 5 = up) CENTRAL_SEQ_CP 0x%08x/0x%08x (st 0x%02x) "
                    "CP_STAT 0x%08x CP_DEBUG 0x%08x",
                 pr->reset_seq, pr->reset_seq & 7, pr->central_cfg, pr->central_st, (pr->central_st >> 16) & 0xFF,
                 pr->cp_stat, pr->cp_debug);
    else
        sbm_logf(p, "PMU: not readable");
}

void sbm_probe_run(const struct sbm_plat *p, unsigned steps, struct sbm_probe *pr)
{
    sbm_memset(pr, 0, sizeof(*pr));
    pr->steps = steps;
    sbm_memmap_parse(0, &pr->mm);
    if (!p || !p->probe32)
        return;
    if (steps & (SBM_PROBE_REGS | SBM_PROBE_SHM)) {
        const uint64_t b = SBM_PMU_PA;
        sbm_logf(p, "probe: reading PMU 0x%08x", (unsigned)b);
        pr->pmu_ok = rd(p, pr, b + SBM_PMU_RESET_SEQ_STATUS, &pr->reset_seq) &&
                     rd(p, pr, b + SBM_PMU_CP_STAT, &pr->cp_stat) &&
                     rd(p, pr, b + SBM_PMU_CP_DEBUG, &pr->cp_debug) &&
                     rd(p, pr, b + SBM_PMU_CENTRAL_SEQ_CP_CFG, &pr->central_cfg) &&
                     rd(p, pr, b + SBM_PMU_CENTRAL_SEQ_CP_ST, &pr->central_st);
        log_pmu(p, pr);          /* in the log before the next block is touched */
        const uint64_t m = SBM_MBOX_PA;
        sbm_logf(p, "probe: reading mailbox 0x%08x", (unsigned)m);
        pr->mbox_ok = rd(p, pr, m + SBM_MBOX_MCUCTLR, &pr->mcuctlr);
        if (pr->mbox_ok) {
            rd(p, pr, m + SBM_MBOX_INTMR0, &pr->intmr0);
            rd(p, pr, m + SBM_MBOX_INTSR0, &pr->intsr0);
            rd(p, pr, m + SBM_MBOX_INTMSR0, &pr->intmsr0);
            rd(p, pr, m + SBM_MBOX_INTMR1, &pr->intmr1);
            rd(p, pr, m + SBM_MBOX_INTSR1, &pr->intsr1);
            rd(p, pr, m + SBM_MBOX_INTMSR1, &pr->intmsr1);
            for (unsigned i = 0; i < SBM_MBOX_NSSR; ++i)
                if (rd(p, pr, m + SBM_MBOX_ISSR(i), &pr->issr[i]))
                    pr->issr_ok |= 1U << i;
        }
    }
    if (steps & SBM_PROBE_SHM) {
        pr->shm_tried = 1;
        const uint64_t c = SBM_MODEM_IF_PA;
        sbm_logf(p, "probe: reading modem_if+0 (0x%08x, 16 bytes)", (unsigned)c);
        pr->shm_head_ok = rd_bytes(p, pr, c, pr->head, sizeof(pr->head));
        if (pr->shm_head_ok) {
            sbm_logf(p, "probe: reading modem_if+0x2a0 (memory map, %u bytes)", (unsigned)SBM_MEMMAP_BYTES);
            pr->shm_map_ok = rd_bytes(p, pr, c + SBM_MEMMAP_OFF, pr->map_raw, SBM_MEMMAP_BYTES);
        }
        if (pr->shm_map_ok)
            sbm_memmap_parse(pr->map_raw, &pr->mm);
        if (pr->shm_map_ok && pr->mm.ipc_size >= 8) {
            sbm_logf(p, "probe: reading IPC region modem_if+0x%08x (8 bytes)", pr->mm.ipc_off);
            pr->shm_ipc_ok = rd(p, pr, c + pr->mm.ipc_off, &pr->ipc_magic) &&
                             rd(p, pr, c + pr->mm.ipc_off + 4, &pr->ipc_access);
        }
        /* all zero with no fault: an empty carve-out and a TZASC read-as-zero denial look the same */
        if (pr->shm_head_ok) {
            unsigned nz = 0;
            for (unsigned i = 0; i < sizeof(pr->head); ++i)
                nz |= pr->head[i];
            pr->shm_zero = !nz && (!pr->shm_map_ok || !pr->mm.valid);
        }
        sbm_logf(p, "probe: carve-out reads done");
    }
}

const char *sbm_probe_verdict(const struct sbm_probe *pr)
{
    if (!pr->pmu_ok)
        return "unreadable";
    if (pr->shm_zero)
        return "shm-zero-ambiguous";
    if (pr->shm_ipc_ok && pr->ipc_magic == SBM_CRASH_MAGIC)
        return "cp-crashed";
    if ((pr->reset_seq & 7) != 5)
        return ((pr->central_st >> 16) & 0xFF) == 0x80 ? "cp-in-reset" : "cp-off";
    if (pr->shm_ipc_ok && pr->ipc_magic == SBM_IPC_MAGIC && pr->ipc_access == 1)
        return "cp-online";
    if (pr->shm_ipc_ok && pr->ipc_magic == SBM_BOOT_MAGIC)
        return "cp-booting";
    if (pr->mbox_ok && (pr->issr_ok & (1U << SBM_MBX_CP2AP_STATUS)) && SBM_CP_STATUS(pr->issr[SBM_MBX_CP2AP_STATUS]))
        return "cp-running";
    return "cp-powered";
}

static const char *magic_name(uint32_t v)
{
    switch (v) {
    case SBM_IPC_MAGIC: return "IPC";
    case SBM_BOOT_MAGIC: return "BOOT";
    case SBM_DUMP_MAGIC: return "DUMP";
    case SBM_CRASH_MAGIC: return "CRASH";
    case 0: return "zero";
    default: return "other";
    }
}

void sbm_probe_report(const struct sbm_plat *p, const struct sbm_probe *pr)
{
    if (!p || !p->log)
        return;
    sbm_logf(p, "modem probe: verdict %s, %u faulted read(s)", sbm_probe_verdict(pr), pr->faults);
    log_pmu(p, pr);
    if (pr->mbox_ok) {
        sbm_logf(p, "MBOX: MCUCTLR 0x%08x INTMR0 0x%08x INTSR0 0x%08x INTMSR0 0x%08x INTMR1 0x%08x INTSR1 0x%08x "
                    "INTMSR1 0x%08x",
                 pr->mcuctlr, pr->intmr0, pr->intsr0, pr->intmsr0, pr->intmr1, pr->intsr1, pr->intmsr1);
        sbm_logf(p, "MBOX: ISSR0-7 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x (read mask 0x%04x)",
                 pr->issr[0], pr->issr[1], pr->issr[2], pr->issr[3], pr->issr[4], pr->issr[5], pr->issr[6],
                 pr->issr[7], pr->issr_ok);
        sbm_logf(p, "MBOX: ISSR8-15 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                 pr->issr[8], pr->issr[9], pr->issr[10], pr->issr[11], pr->issr[12], pr->issr[13], pr->issr[14],
                 pr->issr[15]);
        uint32_t a = pr->issr[SBM_MBX_AP2CP_STATUS], c = pr->issr[SBM_MBX_CP2AP_STATUS];
        sbm_logf(p, "MBOX: AP status: wakelock %u ap_status %u pda_active %u sys_rev %u ds_det %u | CP status: "
                    "cp_status %u lte_active %u wakelock %u rat_mode %u | CP2AP msg 0x%04x",
                 SBM_AP_WAKELOCK(a), SBM_AP_STATUS(a), SBM_AP_PDA_ACTIVE(a), SBM_AP_SYS_REV(a), SBM_AP_DS_DET(a),
                 SBM_CP_STATUS(c), SBM_CP_LTE_ACTIVE(c), SBM_CP_WAKELOCK(c), SBM_CP_RAT_MODE(c),
                 pr->issr[SBM_MBX_CP2AP_MSG] & 0xFFFF);
    } else {
        sbm_logf(p, "MBOX: not readable (clock off or access refused)");
    }
    if (!pr->shm_tried) {
        sbm_logf(p, "SHM: not tried (step 1 only)");
        return;
    }
    if (!pr->shm_head_ok) {
        sbm_logf(p, "SHM: modem_if + 0 refused the read: the carve-out is not AP-readable now");
        return;
    }
    char hex[64];
    size_t o = 0;
    for (unsigned i = 0; i < sizeof(pr->head); ++i)
        o += (size_t)sbm_fmt(hex + o, sizeof(hex) - o, "%02x%s", pr->head[i], i + 1 < sizeof(pr->head) ? " " : "");
    sbm_logf(p, "SHM: modem_if + 0: %s", hex);
    if (pr->shm_zero)
        sbm_logf(p, "SHM: all zero: nothing loaded OR TZASC read-as-zero; not proof of access");
    if (!pr->shm_map_ok) {
        sbm_logf(p, "SHM: memory map at + 0x2a0 refused the read");
        return;
    }
    const struct sbm_memmap *m = &pr->mm;
    sbm_logf(p, "SHM: memory map id '%s' %s: dram 0x%08x ext 0x%08x bins %u", m->id, m->valid ? "valid" : "INVALID",
             m->dram_size, m->ext_mem_size, m->ext_bin_count);
    for (unsigned i = 0; m->valid && i < m->ext_bin_count; ++i)
        sbm_logf(p, "SHM:   %s at 0x%08x size 0x%08x", m->bin[i].tag, m->bin[i].addr, m->bin[i].size);
    if (pr->shm_ipc_ok)
        sbm_logf(p, "SHM: IPC region + 0: magic 0x%08x (%s) access 0x%08x", pr->ipc_magic, magic_name(pr->ipc_magic),
                 pr->ipc_access);
    else
        sbm_logf(p, "SHM: IPC region refused the read");
}
