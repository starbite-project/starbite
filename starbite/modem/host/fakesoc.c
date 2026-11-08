/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: the fake SoC for stage 3 (fakesoc.h). Behaviour follows this phone's log and PMUCAL's tables:
 * CP_PWRON + CP_START -> the reset sequencer reads 5 and the GIC bit pends; mode 2 -> 0x0b; mode 0 checks the
 * carve-out's BOOT/MAIN against the "signed" CRCs -> 0 or 0x0e; AP2CP_CFG = 1 after a valid mode 0 -> the CP sets
 * cp_status 1 in ISSR3 30 ms later; CENTRAL_SEQ_CP_CONFIGURATION bit 16 = 0 -> status 0x80 (in reset). */
#include <stdlib.h>
#include <string.h>
#include "fakesoc.h"

#define PMU_RSEQ (SBM_PMU_RESET_SEQ_STATUS / 4)
#define PMU_CCFG (SBM_PMU_CENTRAL_SEQ_CP_CFG / 4)
#define PMU_CST (SBM_PMU_CENTRAL_SEQ_CP_ST / 4)

void fakesoc_init(struct fakesoc *s, const struct sbm_plan *plan, uint32_t boot_crc, uint32_t main_crc)
{
    memset(s, 0, sizeof(*s));
    s->pmu[PMU_CCFG] = 1u << 16;        /* as T1 read it */
    s->pmu[SBM_PMU_CP_STAT / 4] = 1;
    s->ctrl[SBM_CP_CTRL_NS] = 0x002016a0;   /* as T4 read it */
    s->boot_crc = boot_crc;
    s->main_crc = main_crc;
    s->boot_size = plan->boot_size;
    s->main_size = plan->main_size;
    for (unsigned i = 0; i < plan->n && i < FS_REGIONS; ++i) {
        s->reg[i].off = plan->l[i].dst_off;
        s->reg[i].size = plan->l[i].size;
        s->reg[i].mem = calloc(1, plan->l[i].size);
    }
    s->nreg = plan->n;
    s->ipc_size = SBM_DT_IPC_SIZE;
    s->ipc = calloc(1, s->ipc_size);
}

void fakesoc_free(struct fakesoc *s)
{
    for (unsigned i = 0; i < s->nreg; ++i)
        free(s->reg[i].mem);
    s->nreg = 0;
    free(s->ipc);
    s->ipc = 0;
}

static uint8_t *carve(struct fakesoc *s, uint32_t off, uint32_t len)
{
    for (unsigned i = 0; i < s->nreg; ++i)
        if (off >= s->reg[i].off && (uint64_t)off + len <= (uint64_t)s->reg[i].off + s->reg[i].size)
            return s->reg[i].mem + (off - s->reg[i].off);
    return 0;
}

/* the fake CP's link side --------------------------------------------------------------------- */
static uint32_t l32(struct fakesoc *s, uint32_t o) { return sbm_le32(s->ipc + o); }
static void cp_msg(struct fakesoc *s, uint32_t v)
{
    s->mbox[SBM_MBOX_ISSR(SBM_MBX_CP2AP_MSG) / 4] = v;
    s->mbox[SBM_MBOX_INTSR0 / 4] |= 1u << 16;
}
static void raw_rx_push(struct fakesoc *s, uint32_t word)
{
    uint8_t f[8] = { 0xFC, SBM_CH_BOOT0, 8, 0 };
    sbm_put32(f + 4, word);
    uint32_t in = l32(s, SBM_LEG_RAW_RX_HEAD);
    for (unsigned i = 0; i < 8; ++i)
        s->ipc[SBM_LEG_RAW_RX_BUFF + (in + i) % SBM_LEG_RAW_RX_SIZE] = f[i];
    sbm_put32(s->ipc + SBM_LEG_RAW_RX_HEAD, (in + 8) % SBM_LEG_RAW_RX_SIZE);
    cp_msg(s, SBM_INT_VALID | 0x10);     /* a data message, as for RAW */
}
static void link_tick(struct fakesoc *s)
{
    if (!s->cp_running || !s->ipc)
        return;
    uint32_t ap = s->mbox[SBM_MBOX_ISSR(SBM_MBX_AP2CP_STATUS) / 4];
    if (s->link == L_IDLE && l32(s, SBM_LEG_MAGIC) == SBM_BOOT_MAGIC && SBM_AP_STATUS(ap) == 1)
        s->link = L_UDL;
    if (s->link == L_UDL) {
        uint32_t in = l32(s, SBM_LEG_RAW_TX_HEAD), out = l32(s, SBM_LEG_RAW_TX_TAIL);
        while (out != in) {
            uint8_t f[16];
            for (unsigned i = 0; i < 16; ++i)
                f[i] = s->ipc[SBM_LEG_RAW_TX_BUFF + (out + i) % SBM_LEG_RAW_TX_SIZE];
            uint32_t len = sbm_le16(f + 2), total = (len + 7) & ~7u;
            if (s->n_udl < 4)
                for (unsigned i = 0; i < 16; ++i)
                    s->udl_frames[s->n_udl][i] = i < total ? f[i] : 0;
            s->n_udl++;
            out = (out + total) % SBM_LEG_RAW_TX_SIZE;
            sbm_put32(s->ipc + SBM_LEG_RAW_TX_TAIL, out);
            uint32_t w = sbm_le32(f + 4);
            if (s->udl_silent || f[1] != SBM_CH_BOOT0)
                continue;
            if (w == SBM_UDL_BOOT_DONE)
                raw_rx_push(s, s->udl_wrong ? 0xBAD0 : SBM_UDL_BOOT_DONE_OK);
            else if (w == SBM_UDL_FIN) {
                raw_rx_push(s, SBM_UDL_FIN_OK);
                s->link = L_INIT_SOON;
                s->link_at_us = s->clock_us + 40000;
            }
        }
    }
    if (s->link == L_INIT_SOON && s->clock_us >= s->link_at_us && !(s->mbox[SBM_MBOX_INTSR0 / 4] & (1u << 16))) {
        cp_msg(s, SBM_CMD2INT(SBM_CMD_INIT_START));
        s->link = L_WAIT_PIF;
    }
    if (s->link == L_PHONE_SOON && s->clock_us >= s->link_at_us && !(s->mbox[SBM_MBOX_INTSR0 / 4] & (1u << 16))) {
        cp_msg(s, SBM_CMD2INT(s->crash_after_pif ? SBM_CMD_CRASH_RESET : SBM_CMD_PHONE_START));
        s->link = L_DONE;
    }
}

static void ap_msg(struct fakesoc *s)
{
    uint32_t v = s->mbox[SBM_MBOX_ISSR(SBM_MBX_AP2CP_MSG) / 4];
    if (s->n_ap_msgs < 32)
        s->ap_msgs[s->n_ap_msgs] = v;
    s->n_ap_msgs++;
    if (s->link == L_WAIT_PIF && v == SBM_CMD2INT(SBM_CMD_PIF_INIT_DONE)) {
        struct sbm_sbd cp;
        s->layout_ok = sbm_sbd_attach(&cp, s->ipc, s->ipc_size) == 0 && cp.n == 9 && cp.rbps_off == 3852;
        if (s->layout_ok) {
            s->link = L_PHONE_SOON;
            s->link_at_us = s->clock_us + 30000;
        }
    }
}

static void tick(struct fakesoc *s)
{
    link_tick(s);
    uint32_t pwr = s->ctrl[SBM_CP_CTRL_NS] & 2, start = s->ctrl[SBM_CP_CTRL_S] & 8;
    if (pwr && start && (s->pmu[PMU_CCFG] & (1u << 16))) {
        s->pmu[PMU_RSEQ] = 5;
        s->gicd_pend |= 1u << 28;
    }
    if (!(s->pmu[PMU_CCFG] & (1u << 16)) && (s->ctrl[SBM_CP_CTRL_NS] & 4)) {
        s->pmu[PMU_CST] = 0x80u << 16;
        s->pmu[PMU_RSEQ] = 0;
        s->cp_running = 0;
    }
    if (s->cp_running && !s->never_status && s->clock_us >= s->run_at_us)
        s->mbox[SBM_MBOX_ISSR(SBM_MBX_CP2AP_STATUS) / 4] |= 1u << 1;
}

static int probe(void *ctx, uint64_t pa, uint32_t *v)
{
    struct fakesoc *s = ctx;
    tick(s);
    *v = 0;
    if (pa >= SBM_PMU_PA && pa < SBM_PMU_PA + sizeof(s->pmu)) {
        if (pa == SBM_PMU_PA + 0x30 || pa == SBM_PMU_PA + 0x34)
            return 0;                   /* secure: an AP read faults */
        *v = s->pmu[(pa - SBM_PMU_PA) / 4];
        return 1;
    }
    if (pa >= SBM_MBOX_PA && pa < SBM_MBOX_PA + SBM_MBOX_SIZE) {
        *v = s->mbox[(pa - SBM_MBOX_PA) / 4];
        return 1;
    }
    if (pa == SBM_AP2CP_CFG_PA) {
        *v = s->ap2cp_cfg;
        return 1;
    }
    if (pa == SBM_GICD_CP_PEND) {
        *v = s->gicd_pend;
        return 1;
    }
    return 0;
}

static int mmio_wr(void *ctx, uint64_t pa, uint32_t v)
{
    struct fakesoc *s = ctx;
    if (s->nwr < FS_MAXREC) {
        s->wr[s->nwr].pa = pa;
        s->wr[s->nwr].v = v;
    }
    s->nwr++;
    if (pa >= SBM_PMU_PA && pa < SBM_PMU_PA + sizeof(s->pmu)) {
        if (pa == SBM_PMU_PA + 0x30 || pa == SBM_PMU_PA + 0x34)
            return 0;
        s->pmu[(pa - SBM_PMU_PA) / 4] = v;
    } else if (pa >= SBM_MBOX_PA && pa < SBM_MBOX_PA + SBM_MBOX_SIZE) {
        if (pa == SBM_MBOX_PA + SBM_MBOX_INTGR1) {
            if (v & 1)
                ap_msg(s);
        } else if (pa == SBM_MBOX_PA + SBM_MBOX_INTCR0) {
            s->mbox[SBM_MBOX_INTSR0 / 4] &= ~v;
        } else {
            s->mbox[(pa - SBM_MBOX_PA) / 4] = v;
        }
    } else if (pa == SBM_AP2CP_CFG_PA) {
        s->ap2cp_cfg = v;
        if ((v & 1) && s->validated && s->pmu[PMU_RSEQ] == 5) {
            s->cp_running = 1;
            s->run_at_us = s->clock_us + 30000;
        }
    } else {
        return 0;
    }
    tick(s);
    return 1;
}

static uint64_t smc(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct fakesoc *s = ctx;
    uint32_t ret = 0xFFFF;
    if (id == SBM_SMC_CP && a1 == SBM_SMC_READ_CTRL && a2 <= 1 && a3 <= 1) {
        uint32_t v = s->ctrl[a3];
        ret = (a2 ? v >> 16 : v & 0xFFFF) << 16;
    } else if (id == SBM_SMC_CP && a1 == SBM_SMC_WRITE_CTRL && a3 <= 1) {
        if (s->refuse_el3_write) {
            ret = 1;
        } else {
            s->ctrl[a3] = (uint32_t)a2;
            ret = 0;
        }
    } else if (id == SBM_SMC_SSS_CLK && a1 <= 1 && !a2 && !a3) {
        s->sss_on = a1 == 0;
        ret = 0;
    } else if (id == SBM_SMC_CP && a1 == SBM_SEC_REINIT && !a2 && !a3) {
        ret = s->sss_on ? SBM_SEC_NOT_WORKING : 0x10;
    } else if (id == SBM_SMC_CP && a1 == (SBM_SEC_NORMAL | SBM_MODEM_IF_PA)) {
        uint8_t *b = carve(s, 0, s->boot_size), *m = carve(s, 0x10000, s->main_size);
        if (!s->sss_on || a2 != s->boot_size || a3 != s->main_size)
            ret = 0x13;
        else if (!b || !m || sbm_crc32(0, b, s->boot_size) != s->boot_crc || sbm_crc32(0, m, s->main_size) != s->main_crc)
            ret = 0x0e;             /* CP_NOT_VALID_MAGIC */
        else {
            ret = 0;
            s->validated = 1;
        }
    }
    if (s->nsmc < FS_MAXREC) {
        s->smc[s->nsmc].a[0] = id;
        s->smc[s->nsmc].a[1] = a1;
        s->smc[s->nsmc].a[2] = a2;
        s->smc[s->nsmc].a[3] = a3;
        s->smc[s->nsmc].ret = ret;
    }
    s->nsmc++;
    tick(s);
    return ret;
}

static int cw(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    struct fakesoc *s = ctx;
    uint8_t *d = carve(s, off, len);
    s->carve_writes++;
    if (!d || s->refuse_carve) {
        s->carve_refused++;
        return 0;
    }
    memcpy(d, src, len);
    return 1;
}

static int cr(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    struct fakesoc *s = ctx;
    uint8_t *d = carve(s, off, len);
    if (!d)
        return 0;
    memcpy(dst, d, len);
    return 1;
}

static void udelay(void *ctx, uint32_t us)
{
    struct fakesoc *s = ctx;
    s->clock_us += us;
    tick(s);
}
static uint64_t now_us(void *ctx) { return ((struct fakesoc *)ctx)->clock_us; }

void fakesoc_plat(struct fakesoc *s, struct sbm_plat *p, void (*log)(void *, const char *))
{
    memset(p, 0, sizeof(*p));
    p->ctx = s;
    p->probe32 = probe;
    p->log = log;
    p->smc = smc;
    p->mmio_wr32 = mmio_wr;
    p->carve_write = cw;
    p->carve_read = cr;
    p->udelay = udelay;
    p->now_us = now_us;
    p->ipc = s->ipc;
    if (s->warm_pwron)                   /* after a warm reboot from Android: PWRON kept, sequencer 0 */
        s->ctrl[SBM_CP_CTRL_NS] = 0x002016a2;
    if (s->started_before) {
        s->pmu[PMU_RSEQ] = 5;
        s->ctrl[SBM_CP_CTRL_S] = 8;
    }
}
