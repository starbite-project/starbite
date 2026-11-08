/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: stage 4, the link bring-up up to (not including) INIT_END (docs/MODEM.md section 7). Android's order on
 * this phone (dmesg 2026-11-08 091233, 19.2105 .. 20.49; Samsung's link_device_shmem.c shmem_start_download,
 * xmit_udl, cmd_init_start_handler; modem_ctrl_ss310ap.c boot_on / boot_confirm / boot_off):
 *   DL_START     the legacy map's queue pointers to 0, magic "BOOT" at IPC + 0 (read back), ap_status = 1
 *   UDL          two request/answer pairs on umts_boot0 (ch 215) through the legacy RAW queue: 0x900D -> 0xA00D,
 *                0x9F00 -> 0xAF00 (cbd's std_udl_req_resp)
 *   BOOT_OFF     dump_pc_no_pg off (CP_CTRL_NS bit 17 = 0, via EL3), then wait for the CP's message
 *   INIT_START   the CP asks for the link: lay out the SBD rings (sbm_sbd_init: what init_sbd_link writes) and
 *                answer PIF_INIT_DONE
 *   PHONE_START  the CP is ready for IPC. Android answers INIT_END once RIL has its channels open; stage 4
 *                stops here and parks the CP (sbm_boot_run): no INIT_END, so the CP never goes online (no RIL,
 *                no network, no SIM use, no call).
 * Commands travel as mailbox words: CP -> AP in ISSR1 with INTSR0 bit 16 (acknowledged through INTCR0), AP -> CP
 * in ISSR0 with INTGR1 bit 0. Everything here is polled: no interrupt is enabled.
 */
#include "sbm.h"

#define MB(o) (SBM_MBOX_PA + (o))

static void bar(const struct sbm_plat *p)
{
    if (p->barrier)
        p->barrier(p->ctx);
}
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
static volatile uint32_t *leg(const struct sbm_plat *p, uint32_t off) { return (volatile uint32_t *)(void *)(p->ipc + off); }

/* one CP -> AP message, if the message interrupt is raised: acknowledged, value returned (1), else 0 */
static int msg_take(const struct sbm_plat *p, uint32_t *v)
{
    uint32_t sr = 0;
    if (!p->probe32(p->ctx, MB(SBM_MBOX_INTSR0), &sr) || !(sr & (1u << (16 + SBM_IRQ_CP2AP_MSG))))
        return 0;
    p->probe32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_CP2AP_MSG)), v);
    *v &= 0xFFFF;
    p->mmio_wr32(p->ctx, MB(SBM_MBOX_INTCR0), 1u << (16 + SBM_IRQ_CP2AP_MSG));
    return 1;
}

static int msg_send(const struct sbm_plat *p, uint16_t v)
{
    return p->mmio_wr32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_AP2CP_MSG)), v) &&
           p->mmio_wr32(p->ctx, MB(SBM_MBOX_INTGR1), 1u << SBM_INT_AP2CP_MSG);
}

static void note_msg(const struct sbm_plat *p, struct sbm_boot *b, uint32_t v, const char *during)
{
    b->n_msgs++;
    b->last_msg = v;
    if ((v & SBM_INT_VALID) && (v & SBM_CMD_VALID)) {
        uint32_t cmd = v & SBM_CMD_FIELD;
        if (cmd == SBM_CMD_CRASH_RESET || cmd == SBM_CMD_CRASH_EXIT)
            b->crash_cmd = cmd;
        sbm_logf(p, "link: CP command 0x%02x (msg 0x%04x) during %s", cmd, v, during);
    } else {
        sbm_logf(p, "link: CP message 0x%04x during %s", v, during);
    }
}

/* ---- the legacy RAW queue (UDL) -------------------------------------------------------------- */

static void circ_put(uint8_t *buf, uint32_t size, uint32_t at, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        buf[(at + i) % size] = src[i];
}
static void circ_get(const uint8_t *buf, uint32_t size, uint32_t at, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = buf[(at + i) % size];
}

static int udl_send(const struct sbm_plat *p, uint32_t cmd)
{
    uint8_t f[16] = { 0 }, pl[SBM_UDL_PAYLOAD];
    sbm_memset(pl, 0, sizeof(pl));
    sbm_put32(pl, cmd);
    int l = sbm_sipc5_build(f, sizeof(f), SBM_CH_BOOT0, pl, sizeof(pl));
    if (l < 0)
        return l;
    f[0] |= SBM_SIPC5_PADDING;                       /* the boot link is aligned (BOOT_ALIGNED) */
    uint32_t total = ((uint32_t)l + SBM_SIPC5_ALIGN - 1) & ~(uint32_t)(SBM_SIPC5_ALIGN - 1);
    uint32_t in = *leg(p, SBM_LEG_RAW_TX_HEAD), out = *leg(p, SBM_LEG_RAW_TX_TAIL);
    if (in >= SBM_LEG_RAW_TX_SIZE || out >= SBM_LEG_RAW_TX_SIZE)
        return SBM_EIO;
    uint32_t space = in < out ? out - in - 1 : SBM_LEG_RAW_TX_SIZE + out - in - 1;
    if (space < total)
        return SBM_ENOSPC;
    circ_put(p->ipc + SBM_LEG_RAW_TX_BUFF, SBM_LEG_RAW_TX_SIZE, in, f, total);
    bar(p);
    *leg(p, SBM_LEG_RAW_TX_HEAD) = (in + total) % SBM_LEG_RAW_TX_SIZE;
    bar(p);
    return 0;
}

/* the next frame of the RAW RX queue: 1 = a frame (*ch, *val = its first payload word), 0 = empty, SBM_E* */
static int udl_recv(const struct sbm_plat *p, uint8_t *ch, uint32_t *val)
{
    uint32_t in = *leg(p, SBM_LEG_RAW_RX_HEAD), out = *leg(p, SBM_LEG_RAW_RX_TAIL);
    if (in >= SBM_LEG_RAW_RX_SIZE || out >= SBM_LEG_RAW_RX_SIZE)
        return SBM_EIO;
    if (in == out)
        return 0;
    bar(p);
    uint32_t used = in > out ? in - out : SBM_LEG_RAW_RX_SIZE - out + in;
    uint8_t f[64];
    uint32_t n = used < sizeof(f) ? used : sizeof(f);
    circ_get(p->ipc + SBM_LEG_RAW_RX_BUFF, SBM_LEG_RAW_RX_SIZE, out, f, n);
    struct sbm_sipc5 fr;
    int e = sbm_sipc5_parse(f, n, &fr);
    uint32_t take = e == 0 ? fr.total : used;        /* a bad frame: drop what is there */
    if (e == 0 && (f[0] & SBM_SIPC5_PADDING))
        take = (fr.frame_len + SBM_SIPC5_ALIGN - 1) & ~(uint32_t)(SBM_SIPC5_ALIGN - 1);
    if (take > used)
        take = used;
    *leg(p, SBM_LEG_RAW_RX_TAIL) = (out + take) % SBM_LEG_RAW_RX_SIZE;
    bar(p);
    if (e < 0)
        return SBM_EBADF;
    *ch = fr.ch;
    *val = fr.payload_len >= 4 ? sbm_le32(fr.payload) : (fr.payload_len >= 2 ? sbm_le16(fr.payload) : 0);
    return 1;
}

static int udl_exchange(const struct sbm_plat *p, struct sbm_boot *b, uint32_t req, uint32_t want, uint32_t *got)
{
    sbm_logf(p, "link: UDL request 0x%04x, expecting 0x%04x", req, want);
    int r = udl_send(p, req);
    if (r < 0)
        return r;
    for (uint32_t ms = 0; ms < 2000; ++ms) {
        uint32_t v;
        uint8_t ch;
        if (msg_take(p, &v))
            note_msg(p, b, v, "UDL");
        if (b->crash_cmd)
            return SBM_EIO;
        r = udl_recv(p, &ch, &v);
        if (r < 0)
            return r;
        if (r == 1) {
            *got = v & 0xFFFF;
            sbm_logf(p, "link: UDL answer 0x%04x on ch %u (%s)", *got, ch, *got == want && ch == SBM_CH_BOOT0 ? "ok" : "UNEXPECTED");
            return *got == want && ch == SBM_CH_BOOT0 ? 0 : SBM_EIO;
        }
        delay(p, 1000);
        if (!(ms % 100))
            feed(p);
    }
    sbm_logf(p, "link: no UDL answer in 2 s");
    return SBM_ETIME;
}

/* ---- the whole stage --------------------------------------------------------------------------- */

static int wait_cmd(const struct sbm_plat *p, struct sbm_boot *b, uint32_t want, uint32_t timeout_ms, uint32_t *ms_out,
                    const char *during)
{
    for (uint32_t ms = 0; ms < timeout_ms; ++ms) {
        uint32_t v;
        if (msg_take(p, &v)) {
            note_msg(p, b, v, during);
            if (b->crash_cmd)
                return SBM_EIO;
            if ((v & (SBM_INT_VALID | SBM_CMD_VALID)) == (SBM_INT_VALID | SBM_CMD_VALID) && (v & SBM_CMD_FIELD) == want) {
                *ms_out = ms;
                return 0;
            }
        }
        delay(p, 1000);
        if (!(ms % 100))
            feed(p);
    }
    return SBM_ETIME;
}

int sbm_link_boot(const struct sbm_plat *p, struct sbm_image *im, struct sbm_boot *b)
{
    if (!p->ipc || im->mm.ipc_size < SBM_LEG_RAW_RX_BUFF + SBM_LEG_RAW_RX_SIZE || !p->mmio_wr32 || !p->probe32)
        return SBM_EPERM;
    /* DL_START */
    sbm_logf(p, "link: DL_START: queue pointers 0, magic BOOT, ap_status 1");
    for (unsigned i = 0; i < SBM_LEG_HEADS; ++i)
        *leg(p, 8 + 4 * i) = 0;
    *leg(p, SBM_LEG_MAGIC) = SBM_BOOT_MAGIC;
    bar(p);
    if (*leg(p, SBM_LEG_MAGIC) != SBM_BOOT_MAGIC) {
        sbm_logf(p, "link: magic did not read back");
        return SBM_EIO;
    }
    uint32_t ap = 0;
    p->probe32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_AP2CP_STATUS)), &ap);
    ap = (ap & ~(0xFu << 1)) | (1u << 1);                   /* ap_status = 1 */
    if (!p->mmio_wr32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_AP2CP_STATUS)), ap))
        return SBM_EPERM;
    /* UDL */
    int r = udl_exchange(p, b, SBM_UDL_BOOT_DONE, SBM_UDL_BOOT_DONE_OK, &b->udl_resp[0]);
    if (r < 0)
        return r;
    r = udl_exchange(p, b, SBM_UDL_FIN, SBM_UDL_FIN_OK, &b->udl_resp[1]);
    if (r < 0)
        return r;
    b->udl_done = 1;
    /* BOOT_OFF */
    sbm_logf(p, "link: BOOT_OFF: dump_pc_no_pg off, waiting for INIT_START (15 s at most)");
    r = sbm_cp_ctrl_rmw(p, SBM_CP_CTRL_NS, 1u << 17, 0, 0);
    if (r < 0)
        return r;
    r = wait_cmd(p, b, SBM_CMD_INIT_START, 15000, &b->init_start_ms, "BOOT_OFF");
    if (r < 0) {
        sbm_logf(p, "link: no INIT_START (%s)", sbm_strerror(r));
        return r;
    }
    b->init_start = 1;
    sbm_logf(p, "link: INIT_START after %u ms: laying out the SBD rings", b->init_start_ms);
    static struct sbm_sbd sbd;
    r = sbm_sbd_init(&sbd, p->ipc, im->mm.ipc_size, sbm_starlte_chans, sbm_starlte_nchans);
    if (r < 0)
        return r;
    bar(p);
    if (!msg_send(p, SBM_CMD2INT(SBM_CMD_PIF_INIT_DONE)))
        return SBM_EPERM;
    b->pif_sent = 1;
    sbm_logf(p, "link: PIF_INIT_DONE sent; waiting for PHONE_START (15 s at most)");
    r = wait_cmd(p, b, SBM_CMD_PHONE_START, 15000, &b->phone_start_ms, "INIT");
    if (r < 0) {
        sbm_logf(p, "link: no PHONE_START (%s)", sbm_strerror(r));
        return r;
    }
    b->phone_start = 1;
    sbm_logf(p, "link: PHONE_START after %u ms. Stage 4 stops here: no INIT_END, the CP is parked", b->phone_start_ms);
    return 0;
}
