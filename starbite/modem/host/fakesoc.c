/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: the fake SoC for stage 3 (fakesoc.h). Behaviour follows this phone's log and PMUCAL's tables:
 * CP_PWRON + CP_START -> the reset sequencer reads 5 and the GIC bit pends; mode 2 -> 0x0b; mode 0 checks the
 * carve-out's BOOT/MAIN against the "signed" CRCs -> 0 or 0x0e; AP2CP_CFG = 1 after a valid mode 0 -> the CP sets
 * cp_status 1 in ISSR3 30 ms later; CENTRAL_SEQ_CP_CONFIGURATION bit 16 = 0 with CP_RESET_SET -> status 0x80 (in
 * reset) and CP_START clear in CP_CTRL_S, the reset sequencer stays 5 (as the phone's parks read it); CP_START
 * written again while in reset (PMUCAL's reset_release) -> status 0, the GIC bit; MCUCTLR bit 0 clears the mailbox. */
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
        s->link_at_us = s->clock_us + 100000;
    }
    /* no INIT_END: the CP asks again every 100 ms (measured on the phone, modemlink-hold 2026-11-08) */
    if (s->link == L_DONE && !s->crash_after_pif && s->clock_us >= s->link_at_us &&
        !(s->mbox[SBM_MBOX_INTSR0 / 4] & (1u << 16))) {
        cp_msg(s, SBM_CMD2INT(SBM_CMD_PHONE_START));
        s->link_at_us = s->clock_us + 100000;
    }
}

/* the CP reads the RFS UL ring: one reply per complete (possibly multi-frame) message */
static void rfs_ul(struct fakesoc *s)
{
    int id = sbm_sbd_id(&s->fcp.sbd, SBM_CH_RFS0);
    const uint8_t *f;
    int n;
    while (id >= 0 && (n = sbm_sbd_get(&s->fcp.sbd, (unsigned)id, SBM_UL, &f, 0)) > 0) {
        struct sbm_sipc5 fr;
        const uint8_t *m;
        uint32_t ml;
        s->rfs_frames_in++;
        if (sbm_sipc5_parse(f, (uint32_t)n, &fr) == 0 && fr.ch == SBM_CH_RFS0 && (uint32_t)n <= 2048 &&
            sbm_reasm_feed(&s->rfs_ra, &fr, &m, &ml) == 1) {
            s->rfs_replies++;
            if (ml >= 6 && sbm_le32(m) == ml) {
                if (m[4] == SBM_RFS_IDX_NV_READ && m[5] == 2 && ml == 15 + 0x1000 && m[6] == 1 &&
                    sbm_le32(m + 7) == 0x2000 && !memcmp(m + 15, s->nv_ref + 0x2000, 0x1000))
                    s->rfs_read_ok++;
                else if (m[4] == SBM_RFS_IDX_NV_WRITE && m[5] == 0 && ml == 15 && m[6] == 1 && sbm_le32(m + 11) == 6000)
                    s->rfs_write_ok++;
                else if (m[4] == SBM_RFS_IDX_OPEN && m[5] == 1 && ml == 14 && sbm_le32(m + 10) == 2)
                    s->rfs_open_enoent++;
                else
                    s->rfs_bad_reply++;
            } else {
                s->rfs_bad_reply++;
            }
        }
        sbm_sbd_get_done(&s->fcp.sbd, (unsigned)id, SBM_UL, 0);
    }
}

static void ap_msg(struct fakesoc *s)
{
    uint32_t v = s->mbox[SBM_MBOX_ISSR(SBM_MBX_AP2CP_MSG) / 4];
    if (s->link == L_ONLINE && v == (SBM_INT_VALID | 0x01)) {
        if (s->cp_mute)
            return;
        rfs_ul(s);
        fakecp_step(&s->fcp);           /* FMT requests: answered from the scenario (no SIM) */
        return;
    }
    if (s->n_ap_msgs < 32)
        s->ap_msgs[s->n_ap_msgs] = v;
    s->n_ap_msgs++;
    if (s->link == L_DONE && v == SBM_CMD2INT(SBM_CMD_INIT_END) &&
        (l32(s, SBM_LEG_MAGIC) != SBM_IPC_MAGIC || l32(s, SBM_LEG_ACCESS) != 1)) {
        /* what T9 (2026-11-09) showed: INIT_END without the IPC link reset -> CRASH_EXIT at once */
        s->link = L_ONLINE;
        cp_msg(s, SBM_CMD2INT(SBM_CMD_CRASH_EXIT));
        s->crashed_no_magic = 1;
    } else if (s->link == L_DONE && v == SBM_CMD2INT(SBM_CMD_INIT_END)) {
        /* online: the NOTIs this phone's CP sent under Android, then RFS requests (read, read, write) */
        s->link = L_ONLINE;
        fakecp_attach(&s->fcp, s->ipc, s->ipc_size, s->sim_present ? FAKE_HOME : FAKE_NOSIM);
        s->fcp.nosim_plmn = s->nosim_plmn;
        s->fcp.regist_seq_quirk = s->regist_seq_quirk;
        fakecp_boot_notis(&s->fcp);
        static const uint8_t rd1[] = { 0xf8, 0xf5, 18, 0, 14, 0, 0, 0, 0x01, 1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00 };
        static const uint8_t rd2[] = { 0xf8, 0xf5, 18, 0, 14, 0, 0, 0, 0x01, 2, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00 };
        uint8_t wr[4 + 6 + 8 + 16] = { 0xf8, 0xf5, 34, 0, 30, 0, 0, 0, 0x02, 3, 0x00, 0x20, 0, 0, 16, 0, 0, 0 };
        for (unsigned i = 0; i < 16; ++i)
            wr[18 + i] = (uint8_t)('0' + i % 10);    /* data that must never be logged */
        if (s->rfs_mode) {
            /* open "CP_OMC_AUDIO_SLSI.bin" (as on the phone), a 6000-byte NV write spread over three 2048-byte
             * buffers (one SIPC5 header, raw continuation: Android's check_more), an NV read of 0x1000 at 0x2000 */
            static const char path[] = "CP_OMC_AUDIO_SLSI.bin";
            uint8_t op[4 + 14 + sizeof(path)];
            uint32_t ol = 14 + sizeof(path);
            op[0] = 0xf8; op[1] = 0xf5; sbm_put16(op + 2, (uint16_t)(4 + ol));
            sbm_put32(op + 4, ol); op[8] = SBM_RFS_IDX_OPEN; op[9] = 1;
            sbm_put32(op + 10, 0); sbm_put32(op + 14, sizeof(path));
            memcpy(op + 18, path, sizeof(path));
            fakecp_raw(&s->fcp, SBM_CH_RFS0, op, sizeof(op));
            static uint8_t big[4 + 14 + 6000];
            uint32_t ml = 14 + 6000;
            big[0] = 0xf8; big[1] = 0xf5; sbm_put16(big + 2, (uint16_t)(4 + ml));
            sbm_put32(big + 4, ml); big[8] = SBM_RFS_IDX_NV_WRITE; big[9] = 0;
            sbm_put32(big + 10, 0x80000); sbm_put32(big + 14, 6000);
            for (unsigned i = 0; i < 6000; ++i)
                big[18 + i] = (uint8_t)(0x5A ^ i);
            /* as the phone does (T10): ordinary SIPC5 frames of 2040 payload bytes, joined by the RFS length */
            const uint8_t *rm = big + 4;
            for (uint32_t o = 0; o < ml; o += 2040) {
                uint8_t fr[2048];
                uint32_t k = ml - o < 2040 ? ml - o : 2040;
                int fl = sbm_sipc5_build(fr, sizeof(fr), SBM_CH_RFS0, rm + o, k);
                fakecp_raw(&s->fcp, SBM_CH_RFS0, fr, (uint32_t)fl);
            }
            uint8_t rd[18] = { 0xf8, 0xf5, 18, 0, 14, 0, 0, 0, SBM_RFS_IDX_NV_READ, 2 };
            sbm_put32(rd + 10, 0x2000); sbm_put32(rd + 14, 0x1000);
            fakecp_raw(&s->fcp, SBM_CH_RFS0, rd, sizeof(rd));
            s->rfs_ra.buf = s->rfs_rbuf;
            s->rfs_ra.cap = sizeof(s->rfs_rbuf);
        } else {
            fakecp_raw(&s->fcp, SBM_CH_RFS0, rd1, sizeof(rd1));
            fakecp_raw(&s->fcp, SBM_CH_RFS0, rd2, sizeof(rd2));
            fakecp_raw(&s->fcp, SBM_CH_RFS0, wr, sizeof(wr));
        }
        cp_msg(s, s->online_crash ? SBM_CMD2INT(SBM_CMD_CRASH_EXIT) : (SBM_INT_VALID | 0x01));
    }
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
        if (pa == SBM_PMU_PA + SBM_PMU_CENTRAL_SEQ_CP_CFG && !(v & (1u << 16)) && (s->ctrl[SBM_CP_CTRL_NS] & 4)) {
            s->pmu[PMU_CST] = 0x80u << 16;      /* reset asserted; the reset sequencer keeps its 5 */
            s->ctrl[SBM_CP_CTRL_S] &= ~(1u << 3); /* and CP_START reads 0 (night-1111: 0x8 -> 0x0 over the park) */
            s->cp_running = 0;
            s->validated = 0;
            s->link = L_IDLE;
            s->asserts++;
        }
    } else if (pa >= SBM_MBOX_PA && pa < SBM_MBOX_PA + SBM_MBOX_SIZE) {
        if (pa == SBM_MBOX_PA + SBM_MBOX_MCUCTLR) {
            if (v & 1) {
                memset(s->mbox, 0, sizeof(s->mbox));
                s->mbox_resets++;
            }
        } else if (pa == SBM_MBOX_PA + SBM_MBOX_INTGR1) {
            if (v & 1)
                ap_msg(s);
        } else if (pa == SBM_MBOX_PA + SBM_MBOX_INTCR0) {
            s->mbox[SBM_MBOX_INTSR0 / 4] &= ~v;
        } else {
            s->mbox[(pa - SBM_MBOX_PA) / 4] = v;
        }
    } else if (pa == SBM_AP2CP_CFG_PA) {
        s->ap2cp_cfg = v;
        if ((v & 1) && s->validated && s->pmu[PMU_RSEQ] == 5 && !(s->pmu[PMU_CST] & (0x80u << 16))) {
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
            if (a3 == SBM_CP_CTRL_S && (a2 & 8) && (s->pmu[PMU_CST] >> 16) == 0x80 && !s->release_stuck) {
                s->pmu[PMU_CST] = 0;            /* PMUCAL's reset_release */
                s->gicd_pend |= 1u << 28;
                s->releases++;
            }
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

static int radio_rd(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    struct fakesoc *s = ctx;
    if (!s->radio || off > s->radio_size || len > s->radio_size - off)
        return -1;
    memcpy(buf, s->radio + off, len);
    s->radio_reads++;
    s->radio_bytes += len;
    if (s->radio_flip_at && s->radio_reads >= s->radio_flip_at && len)
        ((uint8_t *)buf)[0] ^= 1;
    return 0;
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
    if (s->radio)
        p->radio_read = radio_rd;
    if (s->warm_pwron)                   /* after a warm reboot from Android: PWRON kept, sequencer 0 */
        s->ctrl[SBM_CP_CTRL_NS] = 0x002016a2;
    if (s->started_before) {
        s->pmu[PMU_RSEQ] = 5;
        s->ctrl[SBM_CP_CTRL_S] = 8;
    }
}
