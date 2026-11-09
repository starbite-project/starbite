/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: stage 3, loading the CP image and starting the CP (docs/MODEM.md section 6), in Android's order on this
 * phone (dmesg 2026-11-08 091233, lines 2159-2296; Samsung's link_device_shmem.c shmem_security_request,
 * modem_ctrl_ss310ap.c ss310ap_on / ss310ap_off, cal-if pmucal_cp_exynos9810.h):
 *   cp_init      CP_CTRL_NS |= CP_PWRON, CP_CTRL_S |= CP_START (EL3 read-modify-write), 1 ms, wait the reset
 *                sequencer = 5 (and the GIC pending bit, as PMUCAL does)
 *   security 2   SMC 0x82000700 (2, 0, 0) with the SSS clock on around it: Android gets 0x0b, and so should we
 *   copy         BOOT/MAIN/VSS/NV to their places, each read back from DRAM and CRC-checked
 *   security 0   SMC 0x82000700 (0 | 0xF0000000, BOOT size, MAIN size): EL3 checks the signature, 0 = valid
 *   power on     mailbox words 0, ISSR2 = sys_rev / ds_det / pda_active, AP2CP_CFG = 1: the CP CPU runs
 *   park         Android's ss310ap_off: wakeup interrupt, 5 ms, dump_pc_no_pg, cal_cp_reset_assert
 * What it never does: send the CP anything over IPC (no INIT_END, no RIL, so no network search or attach, no
 * SIM use, no call of any kind); write a partition (it has no block I/O); leave a CP it started un-parked.
 * Every step is announced in the log first ("boot: ..."), so a hang shows in pmsg where it was.
 */
#include "sbm.h"

#define PMU(o) (SBM_PMU_PA + (o))

const struct sbm_seq sbm_seq_cp_init[] = {
    { SBM_SEQ_WRITE_EL3, "CP_CTRL_NS |= CP_PWRON", SBM_CP_CTRL_NS, 1u << 1, 1u << 1 },
    { SBM_SEQ_WRITE_EL3, "CP_CTRL_S |= CP_START", SBM_CP_CTRL_S, 1u << 3, 1u << 3 },
    { SBM_SEQ_DELAY, "delay 1024 us", 0, 0, 0x400 },
    { SBM_SEQ_WAIT, "RESET_SEQUENCER_STATUS == 5", PMU(SBM_PMU_RESET_SEQ_STATUS), 0x7, 0x5 },
    { SBM_SEQ_WAIT, "GICD pending bit 28", SBM_GICD_CP_PEND, 1u << 28, 1u << 28 },
};
const unsigned sbm_seq_cp_init_n = sizeof(sbm_seq_cp_init) / sizeof(sbm_seq_cp_init[0]);

const struct sbm_seq sbm_seq_reset_assert[] = {
    { SBM_SEQ_WRITE, "RESET_AHEAD_CP_SYS_PWR_REG", PMU(0x1320), 0x3, 0x0 },
    { SBM_SEQ_WRITE, "CLEANY_BUS_CP_SYS_PWR_REG", PMU(0x1324), 0x1, 0x0 },
    { SBM_SEQ_WRITE, "LOGIC_RESET_CP_SYS_PWR_REG", PMU(0x1328), 0x3, 0x0 },
    { SBM_SEQ_WRITE, "TCXO_GATE_CP_SYS_PWR_REG", PMU(0x132c), 0x1, 0x0 },
    { SBM_SEQ_WRITE, "CP_DISABLE_ISO_SYS_PWR_REG", PMU(0x1330), 0x1, 0x1 },
    { SBM_SEQ_WRITE, "CP_RESET_ISO_SYS_PWR_REG", PMU(0x1334), 0x1, 0x0 },
    { SBM_SEQ_WRITE, "CP_DUMP_PC_SYS_PWR_REG", PMU(0x1338), 0x1, 0x0 },
    { SBM_SEQ_WRITE_EL3, "CP_CTRL_NS |= CP_RESET_SET", SBM_CP_CTRL_NS, 1u << 2, 1u << 2 },
    { SBM_SEQ_WRITE, "CENTRAL_SEQ_CP_CONFIGURATION bit 16 = 0", PMU(SBM_PMU_CENTRAL_SEQ_CP_CFG), 1u << 16, 0 },
    { SBM_SEQ_DELAY, "delay 1024 us", 0, 0, 0x400 },
    { SBM_SEQ_WAIT, "CENTRAL_SEQ_CP_STATUS == 0x80", PMU(SBM_PMU_CENTRAL_SEQ_CP_ST), 0xFFu << 16, 0x80u << 16 },
};
const unsigned sbm_seq_reset_assert_n = sizeof(sbm_seq_reset_assert) / sizeof(sbm_seq_reset_assert[0]);

const struct sbm_seq sbm_seq_dump_pc_no_pg[] = {
    { SBM_SEQ_WRITE_EL3, "CP_CTRL_NS |= dump_pc_no_pg", SBM_CP_CTRL_NS, 1u << 17, 1u << 17 },
};
const unsigned sbm_seq_dump_pc_no_pg_n = 1;

static void delay(const struct sbm_plat *p, uint32_t us)
{
    if (p->udelay)
        p->udelay(p->ctx, us);
}
static void feed(const struct sbm_plat *p)
{
    if (p->feed)
        p->feed(p->ctx);
}
static uint64_t now(const struct sbm_plat *p) { return p->now_us ? p->now_us(p->ctx) : 0; }

int sbm_cp_ctrl_rmw(const struct sbm_plat *p, unsigned reg, uint32_t mask, uint32_t value, uint32_t *out)
{
    uint32_t old, err, v;
    int r = sbm_cp_ctrl_read(p, reg, &old, &err);
    if (r < 0)
        return r;
    v = (old & ~mask) | (value & mask);
    uint32_t ret = (uint32_t)p->smc(p->ctx, SBM_SMC_CP, SBM_SMC_WRITE_CTRL, v, reg);
    if (ret != 0) {
        sbm_logf(p, "boot: EL3 write call for %s refused: 0x%x", reg ? "CP_CTRL_NS" : "CP_CTRL_S", ret);
        return SBM_EIO;
    }
    r = sbm_cp_ctrl_read(p, reg, &v, &err);
    if (r < 0)
        return r;
    if (out)
        *out = v;
    sbm_logf(p, "boot: %s 0x%08x -> 0x%08x", reg ? "CP_CTRL_NS" : "CP_CTRL_S", old, v);
    return (v & mask) == (value & mask) ? 0 : SBM_EIO;
}

int sbm_seq_run(const struct sbm_plat *p, const struct sbm_seq *s, unsigned n, uint32_t timeout_us)
{
    for (unsigned i = 0; i < n; ++i) {
        const struct sbm_seq *q = &s[i];
        sbm_logf(p, "boot: %s", q->name);
        feed(p);
        switch (q->op) {
        case SBM_SEQ_WRITE: {
            uint32_t v;
            if (!p->probe32 || !p->mmio_wr32 || !p->probe32(p->ctx, q->pa, &v))
                return SBM_EPERM;
            v = (v & ~q->mask) | (q->value & q->mask);
            if (!p->mmio_wr32(p->ctx, q->pa, v))
                return SBM_EPERM;
            break;
        }
        case SBM_SEQ_WRITE_EL3: {
            int r = sbm_cp_ctrl_rmw(p, (unsigned)q->pa, q->mask, q->value, 0);
            if (r < 0)
                return r;
            break;
        }
        case SBM_SEQ_DELAY:
            delay(p, q->value);
            break;
        case SBM_SEQ_WAIT: {
            uint32_t v = 0, waited = 0;
            for (;;) {
                if (!p->probe32 || !p->probe32(p->ctx, q->pa, &v))
                    return SBM_EIO;
                if ((v & q->mask) == q->value)
                    break;
                if (waited >= timeout_us) {
                    sbm_logf(p, "boot: timeout: %s (read 0x%08x)", q->name, v);
                    return SBM_ETIME;
                }
                delay(p, 100);
                waited += 100;
            }
            break;
        }
        default:
            return SBM_EINVAL;
        }
    }
    return 0;
}

int sbm_sec_req(const struct sbm_plat *p, unsigned mode, uint32_t boot_size, uint32_t main_size, uint32_t *ret)
{
    *ret = 0xFFFFFFFFu;
    if (!p->smc || (mode != SBM_SEC_NORMAL && mode != SBM_SEC_REINIT))
        return SBM_EPERM;
    uint64_t x1 = mode == SBM_SEC_NORMAL ? (uint64_t)mode | SBM_MODEM_IF_PA : mode;
    uint64_t x2 = mode == SBM_SEC_NORMAL ? boot_size : 0, x3 = mode == SBM_SEC_NORMAL ? main_size : 0;
    sbm_logf(p, "boot: security call mode %u (0x%x, 0x%x, 0x%x)", mode, (unsigned)x1, (unsigned)x2, (unsigned)x3);
    p->smc(p->ctx, SBM_SMC_SSS_CLK, 0, 0, 0);
    *ret = (uint32_t)p->smc(p->ctx, SBM_SMC_CP, x1, x2, x3);
    p->smc(p->ctx, SBM_SMC_SSS_CLK, 1, 0, 0);
    sbm_logf(p, "boot: security call mode %u answered 0x%02x", mode, *ret);
    return 0;
}

/* ---- the image in RAM ------------------------------------------------------------------------- */

static int toc_idx(const struct sbm_toc *t, const char *name)
{
    for (unsigned i = 0; i < t->n; ++i) {
        unsigned k = 0;
        while (name[k] && t->e[i].name[k] == name[k])
            ++k;
        if (!name[k] && !t->e[i].name[k])
            return (int)i;
    }
    return -1;
}

static uint32_t crc_fed(const struct sbm_plat *p, const uint8_t *d, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t o = 0; o < n; o += 0x100000) {
        uint32_t k = n - o < 0x100000 ? n - o : 0x100000;
        c = sbm_crc32(c, d + o, k);
        feed(p);
    }
    return c;
}

int sbm_image_check(const struct sbm_plat *p, struct sbm_image *im)
{
    if (im->plan.n != 4)
        return SBM_ENOENT;
    for (unsigned k = 0; k < im->plan.n; ++k) {
        const struct sbm_load *l = &im->plan.l[k];
        if (!im->data[k])
            return SBM_ENOENT;
        sbm_logf(p, "boot: checking %s (0x%x bytes)", l->name, l->size);
        im->crc[k] = crc_fed(p, im->data[k], l->size);
        if (l->src == SBM_SRC_NV) {
            /* NV has no CRC in the TOC; its content is the phone's own and is never shown */
            sbm_logf(p, "boot: %s: 0x%x bytes from EFS (crc 0x%08x)", l->name, l->size, im->crc[k]);
            continue;
        }
        int i = toc_idx(&im->toc, l->name);
        if (i < 0)
            return SBM_ENOENT;
        int ok = im->crc[k] == im->toc.e[i].crc;
        sbm_logf(p, "boot: %s: crc 0x%08x, TOC 0x%08x: %s", l->name, im->crc[k], im->toc.e[i].crc,
                 ok ? "ok" : "MISMATCH");
        if (!ok)
            return SBM_EIO;
    }
    return 0;
}

static int copy_in(const struct sbm_plat *p, struct sbm_image *im)
{
    static uint8_t buf[4096];          /* read-back buffer (no allocation in the core) */
    if (!p->carve_write || !p->carve_read)
        return SBM_EPERM;
    for (unsigned k = 0; k < im->plan.n; ++k) {
        const struct sbm_load *l = &im->plan.l[k];
        sbm_logf(p, "boot: copy %s to modem_if+0x%08x (0x%x bytes)", l->name, l->dst_off, l->size);
        for (uint32_t o = 0; o < l->size; o += 0x100000) {
            uint32_t n = l->size - o < 0x100000 ? l->size - o : 0x100000;
            if (!p->carve_write(p->ctx, l->dst_off + o, im->data[k] + o, n))
                return SBM_EIO;
            feed(p);
        }
        uint32_t c = 0;
        for (uint32_t o = 0; o < l->size; o += sizeof(buf)) {
            uint32_t n = l->size - o < sizeof(buf) ? l->size - o : sizeof(buf);
            if (!p->carve_read(p->ctx, l->dst_off + o, buf, n))
                return SBM_EIO;
            c = sbm_crc32(c, buf, n);
            if (!(o & 0xFFFFF))
                feed(p);
        }
        sbm_logf(p, "boot: %s read back from DRAM: crc 0x%08x %s", l->name, c, c == im->crc[k] ? "ok" : "MISMATCH");
        if (c != im->crc[k])
            return SBM_EIO;
    }
    return 0;
}

/* ---- park, power on --------------------------------------------------------------------------- */

static uint32_t reset_seq(const struct sbm_plat *p)
{
    uint32_t v = 0xFF;
    if (!p->probe32 || !p->probe32(p->ctx, PMU(SBM_PMU_RESET_SEQ_STATUS), &v))
        return 0xFF;
    return v & 7;
}

int sbm_cp_park(const struct sbm_plat *p)
{
    uint32_t s, ns, err;
    if (!p || !p->smc || !p->mmio_wr32 || !p->probe32)
        return SBM_EPERM;
    uint32_t seq = reset_seq(p);
    if (sbm_cp_ctrl_read(p, SBM_CP_CTRL_S, &s, &err) == 0 && sbm_cp_ctrl_read(p, SBM_CP_CTRL_NS, &ns, &err) == 0 &&
        seq == 0 && !(s & (1u << 3)) && !(ns & (1u << 1))) {
        sbm_logf(p, "boot: park: nothing was started (sequencer 0, CP_START and CP_PWRON clear)");
        return 0;
    }
    sbm_logf(p, "boot: park: wakeup interrupt, 5 ms, dump_pc_no_pg, reset assert (Android's power-off)");
    p->mmio_wr32(p->ctx, SBM_MBOX_PA + SBM_MBOX_INTGR1, 1u << 1);
    delay(p, 5000);
    int r = sbm_seq_run(p, sbm_seq_dump_pc_no_pg, sbm_seq_dump_pc_no_pg_n, 0);
    int r2 = sbm_seq_run(p, sbm_seq_reset_assert, sbm_seq_reset_assert_n, 100000);
    uint32_t st = 0;
    if (p->probe32(p->ctx, PMU(SBM_PMU_CENTRAL_SEQ_CP_ST), &st))
        sbm_logf(p, "boot: park: CENTRAL_SEQ_CP_STATUS 0x%02x (0x80 = in reset), sequencer %u", (st >> 16) & 0xFF,
                 reset_seq(p));
    return r < 0 ? r : r2;
}

static int power_on(const struct sbm_plat *p, struct sbm_boot *b)
{
    uint32_t v = 0;
    sbm_logf(p, "boot: power on: mailbox words 0, AP status (sys_rev %u, ds_det %u, pda_active)", SBM_SYS_REV,
             SBM_DS_DET);
    for (unsigned i = 0; i < SBM_MBOX_WORDS; ++i)
        if (!p->mmio_wr32(p->ctx, SBM_MBOX_PA + SBM_MBOX_ISSR(i), 0))
            return SBM_EPERM;
    uint32_t ap = (uint32_t)SBM_SYS_REV << 6 | (uint32_t)SBM_DS_DET << 14;
    if (!p->mmio_wr32(p->ctx, SBM_MBOX_PA + SBM_MBOX_ISSR(SBM_MBX_AP2CP_STATUS), ap) ||
        !p->mmio_wr32(p->ctx, SBM_MBOX_PA + SBM_MBOX_ISSR(SBM_MBX_AP2CP_STATUS), ap | 1u << 5))
        return SBM_EPERM;
    p->probe32(p->ctx, SBM_AP2CP_CFG_PA, &v);
    sbm_logf(p, "boot: AP2CP_CFG 0x%08x -> 1: the CP CPU starts", v);
    if (!p->mmio_wr32(p->ctx, SBM_AP2CP_CFG_PA, 1))
        return SBM_EPERM;
    b->released = 1;
    p->probe32(p->ctx, SBM_AP2CP_CFG_PA, &v);
    sbm_logf(p, "boot: AP2CP_CFG now 0x%08x", v);
    /* ss310ap_boot_on: cp_status (ISSR3 bits 4:1) != 0, up to 2 s; nothing is sent back */
    uint64_t t0 = now(p);
    for (uint32_t ms = 0; ms <= 2000; ms += 10) {
        uint32_t c = 0;
        p->probe32(p->ctx, SBM_MBOX_PA + SBM_MBOX_ISSR(SBM_MBX_CP2AP_STATUS), &c);
        if (SBM_CP_STATUS(c)) {
            b->cp_status_ms = p->now_us ? (uint32_t)((now(p) - t0) / 1000) : ms;
            break;
        }
        if (ms == 2000) {
            b->cp_status_ms = 0xFFFFFFFFu;
            break;
        }
        delay(p, 10000);
        feed(p);
    }
    for (unsigned i = 0; i < 4; ++i)
        p->probe32(p->ctx, SBM_MBOX_PA + SBM_MBOX_ISSR(i), &b->issr[i]);
    sbm_logf(p, "boot: cp_status %s after %u ms; ISSR0-3 0x%08x 0x%08x 0x%08x 0x%08x",
             b->cp_status_ms == 0xFFFFFFFFu ? "never set" : "set", b->cp_status_ms == 0xFFFFFFFFu ? 2000 : b->cp_status_ms,
             b->issr[0], b->issr[1], b->issr[2], b->issr[3]);
    return b->cp_status_ms == 0xFFFFFFFFu ? SBM_ETIME : 0;
}

/* ---- the whole stage -------------------------------------------------------------------------- */

static int stop(const struct sbm_plat *p, struct sbm_boot *b, int err, const char *at)
{
    b->err = err;
    b->stopped_at = at;
    if (b->inited) {
        int r = sbm_cp_park(p);
        b->parked = r == 0;
    }
    b->reset_seq_after = reset_seq(p);
    return err;
}

int sbm_boot_run(const struct sbm_plat *p, struct sbm_image *im, struct sbm_boot *b)
{
    enum sbm_boot_stop want = b->stop;
    uint32_t hold = b->hold_ms;
    int serve = b->rfs_serve, query = b->fmt_query;
    uint32_t period = b->query_period_ms;
    int keep = b->keep_running;
    sbm_memset(b, 0, sizeof(*b));
    b->stop = want;
    b->hold_ms = hold > 600000 ? 600000 : hold;     /* 10 min at most (the soak) */
    b->rfs_serve = serve;
    b->fmt_query = query;
    b->query_period_ms = period;
    b->keep_running = keep && want == SBM_BOOT_ONLINE;
    b->stopped_at = "-";
    sbm_logf(p, "boot: stage 3 up to %s", want == SBM_BOOT_DRY ? "the RAM check" : want == SBM_BOOT_SEC
             ? "EL3's signature check (the CP CPU is not started)" : want == SBM_BOOT_RUN
             ? "the CP's first status (then parked)" : want == SBM_BOOT_LINK
             ? "stage 4: the link up to PHONE_START (no INIT_END; then parked)"
             : "stage 5a: INIT_END, receive only, then parked");
    int r = sbm_image_check(p, im);
    if (r < 0)
        return stop(p, b, r, "image check");
    b->checked = 1;
    if (want == SBM_BOOT_DRY)
        return stop(p, b, 0, "done (dry)");

    if (!p->smc || !p->mmio_wr32 || !p->probe32 || !p->carve_write || !p->carve_read)
        return stop(p, b, SBM_EPERM, "platform");
    if (want >= SBM_BOOT_LINK && (!p->ipc || im->mm.ipc_size < SBM_LEG_RAW_RX_BUFF + SBM_LEG_RAW_RX_SIZE))
        return stop(p, b, SBM_EPERM, "platform (no IPC region view)");
    /* only a CP that nobody started: the state S-Boot leaves (T1, T4) */
    uint32_t err;
    b->reset_seq_before = reset_seq(p);
    if (sbm_cp_ctrl_read(p, SBM_CP_CTRL_S, &b->ctrl_s, &err) < 0 || sbm_cp_ctrl_read(p, SBM_CP_CTRL_NS, &b->ctrl_ns, &err) < 0)
        return stop(p, b, SBM_EIO, "CP_CTRL read");
    sbm_logf(p, "boot: before: sequencer %u, CP_CTRL_S 0x%08x, CP_CTRL_NS 0x%08x", b->reset_seq_before, b->ctrl_s,
             b->ctrl_ns);
    /* Android's rule (shmem_security_request): cp_init when the sequencer reads 0. A warm reboot resets the
     * sequencer but keeps CP_PWRON in CP_CTRL_NS (T7 21:21 after One UI: 0x002016a2, sequencer 0); cal_cp_init
     * sets it anyway. A running sequencer or CP_START set means a CP that is up: not touched. */
    if (b->reset_seq_before != 0 || (b->ctrl_s & (1u << 3)))
        return stop(p, b, SBM_EPERM, "the CP was already started: not touched");
    if (b->ctrl_ns & (1u << 1))
        sbm_logf(p, "boot: CP_PWRON left set by an earlier boot (a warm reboot keeps it); cp_init runs as on Android");

    b->inited = 1;                       /* from here on, every way out parks */
    r = sbm_seq_run(p, sbm_seq_cp_init, sbm_seq_cp_init_n - 1, 100000);
    if (r < 0)
        return stop(p, b, r, "cp_init");
    if (sbm_seq_run(p, &sbm_seq_cp_init[sbm_seq_cp_init_n - 1], 1, 100000) < 0)
        sbm_logf(p, "boot: (the GIC pending bit did not show: PMUCAL only waits for it; going on)");
    sbm_cp_ctrl_read(p, SBM_CP_CTRL_S, &b->ctrl_s, &err);
    sbm_cp_ctrl_read(p, SBM_CP_CTRL_NS, &b->ctrl_ns, &err);

    sbm_sec_req(p, SBM_SEC_REINIT, 0, 0, &b->sec2);
    if (b->sec2 != SBM_SEC_NOT_WORKING)
        sbm_logf(p, "boot: (mode 2 answered 0x%02x, Android got 0x0b; going on as cbd does)", b->sec2);

    r = copy_in(p, im);
    if (r < 0)
        return stop(p, b, r, "copy");
    b->copied = 1;

    uint64_t t0 = now(p);
    sbm_sec_req(p, SBM_SEC_NORMAL, im->plan.boot_size, im->plan.main_size, &b->sec0);
    b->sec0_us = now(p) - t0;
    if (b->sec0 != SBM_SEC_OK)
        return stop(p, b, SBM_EIO, "EL3 refused the image");
    b->validated = 1;
    if (want == SBM_BOOT_SEC)
        return stop(p, b, 0, "done (validated, not started)");

    r = power_on(p, b);
    if (r < 0 || want == SBM_BOOT_RUN)
        return stop(p, b, r, r < 0 ? "power on" : "done (first status, parked)");
    r = sbm_link_boot(p, im, b);
    if (want == SBM_BOOT_ONLINE && b->keep_running && r == 0 && b->init_end_sent) {
        b->stopped_at = "online (session: running, not parked)";
        return 0;                             /* the session owns the CP now; sbm_modem_stop parks it */
    }
    if (want == SBM_BOOT_ONLINE && b->init_end_sent)
        return stop(p, b, r, r < 0 ? "online (an error or a crash command)" : "done (online, receive only, parked)");
    return stop(p, b, r, r < 0 ? (b->pif_sent ? "waiting for PHONE_START" : b->init_start ? "INIT_START"
                                  : b->udl_done ? "waiting for INIT_START" : "DL_START / UDL")
                               : "done (PHONE_START, no INIT_END, parked)");
}

void sbm_boot_report(const struct sbm_plat *p, const struct sbm_boot *b)
{
    sbm_logf(p, "BOOT: %s: %s (stopped at: %s)", b->err ? "FAILED" : "ok", sbm_strerror(b->err), b->stopped_at);
    sbm_logf(p, "BOOT: checked %u inited %u copied %u validated %u released %u parked %u", b->checked, b->inited,
             b->copied, b->validated, b->released, b->parked);
    if (b->inited)
        sbm_logf(p, "BOOT: EL3 mode 2 0x%02x, mode 0 0x%02x in %u ms; CP_CTRL_S 0x%08x NS 0x%08x", b->sec2, b->sec0,
                 (unsigned)(b->sec0_us / 1000), b->ctrl_s, b->ctrl_ns);
    if (b->released)
        sbm_logf(p, "BOOT: CP status after %u ms; ISSR3 0x%08x (cp_status %u)", b->cp_status_ms, b->issr[3],
                 SBM_CP_STATUS(b->issr[3]));
    if (b->stop >= SBM_BOOT_LINK && b->released)
        sbm_logf(p, "LINK: udl %u (0x%04x 0x%04x) init_start %u (%u ms) pif %u phone_start %u (%u ms) msgs %u crash 0x%x",
                 b->udl_done, b->udl_resp[0], b->udl_resp[1], b->init_start, b->init_start_ms, b->pif_sent,
                 b->phone_start, b->phone_start_ms, b->n_msgs, b->crash_cmd);
    if (b->stop == SBM_BOOT_ONLINE && b->init_end_sent) {
        char line[160];
        static const char *const why[] = { "-", "time", "SIM present", "registration", "crash command", "frame budget" };
        sbm_cell_line_masked(&b->cell, line, sizeof(line));
        sbm_logf(p, "ONLINE: stopped by %s; FMT %u, RFS %u (reads %u, writes %u, other %u), bad %u", why[b->stopped_by <= 5 ? b->stopped_by : 0],
                 b->fmt_rx, b->rfs_rx, b->rfs_reads, b->rfs_writes, b->rfs_other, b->bad_rx);
        sbm_logf(p, "ONLINE: %s", line);
        if (b->fmt_query)
            sbm_logf(p, "ONLINE: queries sent %u (refused by the allow-list %u), answered %u RESP + %u GEN_PHONE_RES",
                     b->fmt_tx, b->fmt_tx_refused, b->fmt_resp, b->fmt_gen_res);
        if (b->rfs_serve || b->big_frames || b->big_dropped)
            sbm_logf(p, "ONLINE: RFS served %u replies (%u frames, %u bytes); joined %u long messages from %u pieces; big frames %u, dropped %u",
                     b->rfs_tx, b->rfs_tx_frames, b->rfs_tx_bytes, b->rfs_joined, b->rfs_parts, b->big_frames, b->big_dropped);
    }
    if (b->stop == SBM_BOOT_LINK && b->hold_ms)
        sbm_logf(p, "HOLD: %u ms: PHONE_START repeated %u times (last gap %u ms), other commands %u, DL frames waiting %u",
                 b->hold_ms, b->phone_start_repeats, b->phone_start_gap_ms, b->other_cmds, b->dl_frames_seen);
    sbm_logf(p, "BOOT: sequencer before %u, after %u", b->reset_seq_before, b->reset_seq_after);
}
