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

static uint32_t g_online_ms = 0xFFFFFFFFu;   /* ms since INIT_END while online (for the log), else ~0 */
static void note_msg(const struct sbm_plat *p, struct sbm_boot *b, uint32_t v, const char *during)
{
    b->n_msgs++;
    if (g_online_ms != 0xFFFFFFFFu)
        sbm_logf(p, "online: +%u ms: CP msg 0x%04x", g_online_ms, v);
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

/* After PHONE_START, optionally: stay, send nothing, and watch. The CP repeats PHONE_START while it waits for
 * INIT_END (Android's log: "rild_ready" not yet); every DL ring's pointers are compared, nothing is consumed. */
static int hold(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd)
{
    uint32_t last = 0;
    sbm_logf(p, "link: holding %u ms before the park: sending nothing, consuming nothing", b->hold_ms);
    for (uint32_t ms = 1; ms <= b->hold_ms; ++ms) {
        uint32_t v;
        if (msg_take(p, &v)) {
            if ((v & 0xFF) == SBM_CMD2INT(SBM_CMD_PHONE_START)) {
                b->phone_start_repeats++;
                b->phone_start_gap_ms = ms - last;
                last = ms;
                if (b->phone_start_repeats <= 3)
                    sbm_logf(p, "link: PHONE_START again at +%u ms", ms);
            } else {
                b->other_cmds++;
                note_msg(p, b, v, "hold");
                if (b->crash_cmd)
                    return SBM_EIO;
            }
        }
        if (!(ms % 1000)) {
            uint32_t waiting = 0;
            for (unsigned id = 0; id < sbd->n; ++id) {
                uint16_t rp = sbm_sbd_rp(sbd, id, SBM_DL), wp = sbm_sbd_wp(sbd, id, SBM_DL);
                uint16_t len = sbd->r[id][SBM_DL].len;
                if (rp < len && wp < len && rp != wp) {
                    uint32_t n = wp > rp ? (uint32_t)wp - rp : (uint32_t)len - rp + wp;
                    waiting += n;
                    if (ms == b->hold_ms || n != 0)
                        sbm_logf(p, "link: +%u s: DL ring ch %u has %u frame(s) waiting (not read)", ms / 1000,
                                 sbd->r[id][SBM_DL].ch, n);
                }
            }
            b->dl_frames_seen = waiting;
            feed(p);
        }
        delay(p, 1000);
    }
    return 0;
}

/* ---- stage 5a: INIT_END, then receive only ---------------------------------------------------- */
/* The CP goes online. Nothing is sent after INIT_END: no FMT request, no RFS answer. FMT frames are decoded (identity-
 * free state, redacted lines); RFS frames show their header and offset/length only, never data. Stops (and the caller
 * parks) on a SIM that is not absent, on any registration, on a crash command, after hold_ms or 200 frames. */
#define ONLINE_LOG_MAX 40
static void log_rfs(const struct sbm_plat *p, struct sbm_boot *b, const uint8_t *pl, uint32_t n, int logit)
{
    if (n < SBM_RFS_HDR) {
        b->bad_rx++;
        return;
    }
    uint32_t len = sbm_le32(pl);
    uint8_t idx = pl[4], id = pl[5];
    uint16_t cmd = SBM_CMD(SBM_G_RFS, idx);
    if (cmd == SBM_RFS_NV_READ_ITEM)
        b->rfs_reads++;
    else if (cmd == SBM_RFS_NV_WRITE_ITEM)
        b->rfs_writes++;
    else
        b->rfs_other++;
    if (!logit)
        return;
    if (idx == SBM_RFS_IDX_OPEN && n >= SBM_RFS_HDR + 8) {
        /* open: u32 flags, u32 path length, the path (a file name in the RFS directory: not an identifier) */
        char path[64];
        uint32_t plen = sbm_le32(pl + 10), k = 0;
        for (; k < plen && k + 1 < sizeof(path) && SBM_RFS_HDR + 8 + k < n; ++k) {
            char c = (char)pl[SBM_RFS_HDR + 8 + k];
            path[k] = c >= 0x20 && c < 0x7F ? c : '?';
        }
        path[k] = 0;
        sbm_logf(p, "RFS: rx idx 0x11 id %u len %u: open flags 0x%x path '%s' (not answered)", id, len, sbm_le32(pl + 6),
                 path);
        return;
    }
    if ((idx == SBM_RFS_IDX_NV_READ || idx == SBM_RFS_IDX_NV_WRITE) && n >= SBM_RFS_HDR + 8)
        sbm_logf(p, "RFS: rx idx 0x%02x id %u len %u: offset 0x%x length 0x%x (%s)", idx, id, len,
                 sbm_le32(pl + 6), sbm_le32(pl + 10), idx == SBM_RFS_IDX_NV_READ ? "NV read" : "NV write, data not shown");
    else
        sbm_logf(p, "RFS: rx idx 0x%02x id %u len %u (%u bytes; body not shown)", idx, id, len, n);
}

/* Android's shmem_reset_ipc_link, run on the CP's first PHONE_START: access 0, magic 0, the legacy queue words 0,
 * then magic IPC (0xAA) and access 1, read back. The CP only treats the link as up (ipc_active) with both set:
 * T9 (2026-11-09 15:58) sent INIT_END with the magic still "BOOT" and the CP answered CRASH_EXIT at once. */
static int reset_ipc_link(const struct sbm_plat *p)
{
    *leg(p, SBM_LEG_ACCESS) = 0;
    *leg(p, SBM_LEG_MAGIC) = 0;
    for (unsigned i = 0; i < SBM_LEG_HEADS; ++i)
        *leg(p, 8 + 4 * i) = 0;
    bar(p);
    *leg(p, SBM_LEG_MAGIC) = SBM_IPC_MAGIC;
    *leg(p, SBM_LEG_ACCESS) = 1;
    bar(p);
    return *leg(p, SBM_LEG_MAGIC) == SBM_IPC_MAGIC && *leg(p, SBM_LEG_ACCESS) == 1 ? 0 : SBM_EIO;
}

static void online_state(const struct sbm_plat *p, const char *when)
{
    uint32_t i1 = 0, i3 = 0, sr = 0;
    p->probe32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_CP2AP_MSG)), &i1);
    p->probe32(p->ctx, MB(SBM_MBOX_ISSR(SBM_MBX_CP2AP_STATUS)), &i3);
    p->probe32(p->ctx, MB(SBM_MBOX_INTSR0), &sr);
    sbm_logf(p, "online: %s: magic 0x%08x access %u | ISSR1 0x%04x ISSR3 0x%08x (cp_status %u) INTSR0 0x%08x", when,
             *leg(p, SBM_LEG_MAGIC), *leg(p, SBM_LEG_ACCESS), i1 & 0xFFFF, i3, SBM_CP_STATUS(i3), sr);
}

/* CP -> AP frames longer than one SBD buffer (Android's check_more): the first buffer carries the SIPC5 header with
 * the whole length, the next buffers are raw continuation without a header, until the length is reached. T9c: an
 * 89,680-byte NV write arrived this way. */
struct bigf {
    uint8_t *buf;
    uint32_t cap, total, rcvd;
    int active;
};
static uint8_t big_fmt[2][0x10000], big_rfs[0xA0000];
static struct bigf bigs[3] = { { big_fmt[0], sizeof(big_fmt[0]), 0, 0, 0 }, { big_fmt[1], sizeof(big_fmt[1]), 0, 0, 0 },
                               { big_rfs, sizeof(big_rfs), 0, 0, 0 } };

/* 1 = a whole frame in (*fb, *fl); 0 = taken into a big frame (more to come); -1 = dropped */
static int big_feed(struct sbm_boot *b, struct bigf *g, const uint8_t *f, uint32_t n, uint32_t bufsz,
                    const uint8_t **fb, uint32_t *fl)
{
    if (g->active) {
        if (n > g->cap - g->rcvd) {
            g->active = 0;
            b->big_dropped++;
            return -1;
        }
        sbm_memcpy(g->buf + g->rcvd, f, n);
        g->rcvd += n;
        if (g->rcvd < g->total)
            return 0;
        g->active = 0;
        b->big_frames++;
        *fb = g->buf;
        *fl = g->total;
        return 1;
    }
    if (n >= 4 && (f[0] & SBM_SIPC5_START) == SBM_SIPC5_START) {
        uint32_t flen = (f[0] & SBM_SIPC5_EXT) && (f[0] & SBM_SIPC5_MULTI) != SBM_SIPC5_MULTI
                        ? (n >= 6 ? sbm_le32(f + 2) : 0) : sbm_le16(f + 2);
        if (flen > bufsz && flen > n) {
            if (flen > g->cap) {
                b->big_dropped++;
                return -1;
            }
            sbm_memcpy(g->buf, f, n);
            g->rcvd = n;
            g->total = flen;
            g->active = 1;
            return 0;
        }
    }
    *fb = f;
    *fl = n;
    return 1;
}

/* AP -> CP on the RFS UL ring, Android's way (misc_write with max_tx_size = the 2048-byte buffer): one SIPC5 frame
 * when it fits, else multi-frame fragments (config 0xFB, { id, more }), each followed by the data doorbell 0x81. */
#define RFS_TX_FRAME 2048u
static int put_wait_ch(const struct sbm_plat *p, struct sbm_sbd *sbd, uint16_t ch, const uint8_t *fr, uint32_t l)
{
    for (uint32_t t = 0; t < 2000; ++t) {
        int r = sbm_sbd_put(sbd, ch, SBM_UL, fr, l, p);
        if (r == 0)
            return msg_send(p, SBM_INT_VALID | 0x0001) ? 0 : SBM_EPERM;
        if (r != SBM_ENOSPC)
            return r;
        delay(p, 1000);             /* the CP has not consumed the ring yet */
        if (!(t % 100))
            feed(p);
    }
    return SBM_ETIME;
}
static int put_wait(const struct sbm_plat *p, struct sbm_sbd *sbd, const uint8_t *fr, uint32_t l)
{
    return put_wait_ch(p, sbd, SBM_CH_RFS0, fr, l);
}

/* our FMT query seqs: 0x41..0xFE, round and round. Answers are matched to the open-query bitmap, not by range:
 * 2ecf7cd counted aseq > 0x40 only, and the u8 seq wrapped through 0x00..0x40 (soak-2: 130 answers uncounted). */
static uint8_t o_mseq = 0x40;
static uint32_t o_open[8];
/* the command each open seq asked for: the phone's CP answers NET_REGIST (CS) with the seq of the NET_REGIST (PS)
 * query sent right after it (night-1110: both RESPs carry 69 for 68 + 69), so an answer whose seq is not open is
 * matched to an open query for the same command (the domain is in the body, so the state was right all along) */
static uint16_t o_qcmd[256];
#define SBM_QSEQ_FIRST 0x41u
#define SBM_QSEQ_LAST  0xFEu

/* T11: the status GETs, through the builder's allow-list (sbm_ipc_frame refuses anything else: no SET/EXEC, nothing
 * that switches RF on, no CALL/SMS/SS/SAT/identity), on FMT0 with the data doorbell. Answers come back as RESP on
 * the DL rings and go through sbm_cell like everything else. */
static int fmt_queries(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd, uint32_t ms, int logit)
{
    static const struct { uint16_t cmd; uint8_t n; uint8_t d[2]; } q[] = {
        { SBM_PWR_PHONE_STATE, 0, { 0 } },       { SBM_SEC_PIN_STATUS, 0, { 0 } },
        { SBM_SEC_SIM_ICC_TYPE, 0, { 0 } },      { SBM_NET_REGIST, 2, { 0xff, SBM_DOMAIN_CS } },
        { SBM_NET_REGIST, 2, { 0xff, SBM_DOMAIN_PS } }, { SBM_NET_SERVING_NETWORK, 0, { 0 } },
        { SBM_NET_PLMN_SEL, 0, { 0 } },          { SBM_DISP_ICON_INFO, 1, { 0x01 } },
        { SBM_DISP_RSSI_INFO, 0, { 0 } },        { SBM_NET_MODE_SEL, 0, { 0 } },
        { SBM_MISC_TIME_INFO, 0, { 0 } },
    };
    uint8_t fr[64];
    for (unsigned i = 0; i < sizeof(q) / sizeof(q[0]); ++i) {
        char nb[12];
        o_mseq = o_mseq >= SBM_QSEQ_LAST || o_mseq < SBM_QSEQ_FIRST ? (uint8_t)SBM_QSEQ_FIRST : (uint8_t)(o_mseq + 1u);
        uint8_t mseq = o_mseq;
        if (o_open[mseq >> 5] & (1u << (mseq & 31)))
            b->fmt_stale++;                       /* that query never got its answer; the seq is reused */
        int l = sbm_ipc_frame(fr, sizeof(fr), SBM_CH_FMT0, mseq, q[i].cmd, SBM_IPC_GET, q[i].d, q[i].n);
        if (l < 0) {
            b->fmt_tx_refused++;
            continue;
        }
        int r = put_wait_ch(p, sbd, SBM_CH_FMT0, fr, (uint32_t)l);
        if (r < 0)
            return r;
        b->fmt_tx++;
        o_open[mseq >> 5] |= 1u << (mseq & 31);
        o_qcmd[mseq] = q[i].cmd;
        if (logit)
            sbm_logf(p, "FMT: +%u TX ch235 | %s GET seq %u", ms, sbm_ipc_name(q[i].cmd, nb), mseq);
    }
    return 0;
}

static int rfs_send(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd, const uint8_t *m, uint32_t len)
{
    static uint8_t fr[RFS_TX_FRAME];
    static uint8_t info_id;
    int l, r;
    if (len + 4 <= RFS_TX_FRAME) {
        l = sbm_sipc5_build(fr, sizeof(fr), SBM_CH_RFS0, m, len);
        if (l < 0)
            return l;
        r = put_wait(p, sbd, fr, (uint32_t)l);
        b->rfs_tx_frames += r == 0;
        return r;
    }
    info_id = (uint8_t)((info_id + 1) & 0x7F);
    for (uint32_t o = 0; o < len; o += RFS_TX_FRAME - 5) {
        uint32_t k = len - o < RFS_TX_FRAME - 5 ? len - o : RFS_TX_FRAME - 5;
        l = sbm_sipc5_build_multi(fr, sizeof(fr), SBM_CH_RFS0, info_id, o + k < len, m + o, k);
        if (l < 0)
            return l;
        if ((r = put_wait(p, sbd, fr, (uint32_t)l)) < 0)
            return r;
        b->rfs_tx_frames++;
    }
    return 0;
}

/* RFS messages longer than one frame (libsec-ril's Nv::ProcessRfsPacket): the CP sends them as several ordinary
 * SIPC5 frames (2040 payload bytes each on this phone, T10); the RFS header's u32 length (at most 0x80000) says how
 * many bytes make one message, and the pieces are appended until it is reached. T10's 89,680-byte NV write. */
static uint8_t rfs_acc_buf[SBM_RFS_MAX_IO + 64];
static uint32_t rfs_acc_total, rfs_acc_rcvd;

/* 1 = a whole RFS message in (*mm, *ml), 0 = a piece taken, -1 = dropped */
static int rfs_join(struct sbm_boot *b, const uint8_t *m, uint32_t n, const uint8_t **mm, uint32_t *ml)
{
    if (!rfs_acc_total) {
        if (n < SBM_RFS_HDR)
            return -1;
        uint32_t total = sbm_le32(m);
        if (total < SBM_RFS_HDR || total > sizeof(rfs_acc_buf))
            return -1;
        if (total <= n) {
            *mm = m;
            *ml = total;
            return 1;
        }
        sbm_memcpy(rfs_acc_buf, m, n);
        rfs_acc_total = total;
        rfs_acc_rcvd = n;
        b->rfs_parts++;
        return 0;
    }
    if (n > rfs_acc_total - rfs_acc_rcvd) {         /* more than the message said: drop it all */
        rfs_acc_total = rfs_acc_rcvd = 0;
        return -1;
    }
    sbm_memcpy(rfs_acc_buf + rfs_acc_rcvd, m, n);
    rfs_acc_rcvd += n;
    b->rfs_parts++;
    if (rfs_acc_rcvd < rfs_acc_total)
        return 0;
    *mm = rfs_acc_buf;
    *ml = rfs_acc_total;
    rfs_acc_total = rfs_acc_rcvd = 0;
    b->rfs_joined++;
    return 1;
}

/* The online stage's state: one instance (the CP is one), shared by the test images' timed loop and the session API. */
static uint8_t o_rbuf[3][8192];
static struct sbm_reasm o_ra[3];
static struct sbm_rfs o_rfs;
static uint8_t o_reply[SBM_RFS_MAX_IO + 64];
static uint32_t o_logged, o_rfs_logged, o_data_msgs;
/* the flood guard: at most this many frames a minute (the soak sees about 70, the RFS start about 100). It was a total
 * of 200 + hold / 50 until c9867c3: a session would have stopped itself after about 600 frames (8 minutes). */
#define ONLINE_FRAMES_PER_MIN 2000u
static uint32_t o_win_ms, o_win_frames;
static struct sbm_sbd *o_sbd;

/* reset the IPC link, wait 100 ms, the clock table, INIT_END (Android's order on PHONE_START) */
static int online_enter(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd, struct sbm_image *im)
{
    rfs_acc_total = rfs_acc_rcvd = 0;
    o_logged = o_rfs_logged = o_data_msgs = 0;
    o_sbd = sbd;
    o_mseq = SBM_QSEQ_FIRST - 1;
    sbm_memset(o_open, 0, sizeof(o_open));
    if (b->rfs_serve) {
        /* the RAM copy of nv_data.bin the glue read from EFS (read only); the CP's writes change only this RAM */
        uint8_t *nv = 0;
        uint32_t nvs = 0;
        for (unsigned k = 0; k < im->plan.n; ++k)
            if (im->plan.l[k].src == SBM_SRC_NV) {
                nv = (uint8_t *)(uintptr_t)im->data[k];
                nvs = im->plan.l[k].size;
            }
        if (!nv)
            return SBM_ENOENT;
        sbm_rfs_init(&o_rfs, nv, nvs);
    }
    for (unsigned k = 0; k < 3; ++k)
        bigs[k].active = 0;
    if (reset_ipc_link(p) < 0) {
        sbm_logf(p, "online: the IPC link reset did not read back");
        return SBM_EIO;
    }
    sbm_logf(p, "online: IPC link reset (magic 0xAA, access 1), as Android does on PHONE_START");
    /* Android sends INIT_END when RIL opens its channels, about 140 ms later: wait 100 ms, keeping the mailbox clear */
    for (unsigned ms = 0; ms < 100; ++ms) {
        uint32_t v;
        if (msg_take(p, &v) && (v & SBM_CMD_VALID) && (v & SBM_CMD_FIELD) != SBM_CMD_PHONE_START) {
            note_msg(p, b, v, "link reset");
            if (b->crash_cmd) {
                b->stopped_by = SBM_STOP_CRASH;
                online_state(p, "crash before INIT_END");
                return SBM_EIO;
            }
        }
        delay(p, 1000);
    }
    int r = sbm_clk_table_write(p->ipc, sbd->ipc_size);
    if (r < 0)
        return r;
    sbm_logf(p, "online: ECT clock table written (%d bytes); INIT_END -> the CP goes online", r);
    bar(p);
    if (!msg_send(p, SBM_CMD2INT(SBM_CMD_INIT_END)))
        return SBM_EPERM;
    b->init_end_sent = 1;
    online_state(p, "INIT_END sent");
    sbm_cell_init(&b->cell);
    b->sim_status = 0xFFFFFFFFu;
    for (unsigned k = 0; k < 3; ++k) {
        o_ra[k].buf = o_rbuf[k];
        o_ra[k].cap = sizeof(o_rbuf[k]);
        o_ra[k].len = 0;
        o_ra[k].active = 0;
    }
    o_win_ms = 0;
    o_win_frames = 0;
    return 0;
}

/* One pass at time ms since INIT_END: the mailbox, the FMT/RFS rings (RFS answered when serving), the guards, the
 * periodic queries. 0 = go on, 1 = stopped by a guard (b->stopped_by), SBM_E* = an error or a crash command. */
static int online_poll(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd, uint32_t ms)
{
    uint32_t v;
    g_online_ms = ms;
    if (msg_take(p, &v)) {
        if (v & SBM_CMD_VALID) {
            note_msg(p, b, v, "online");
            if (b->crash_cmd) {
                b->stopped_by = SBM_STOP_CRASH;
                g_online_ms = 0xFFFFFFFFu;
                online_state(p, "after the crash command");
                return SBM_EIO;
            }
        } else if (o_data_msgs++ < 5) {
            sbm_logf(p, "online: +%u ms: CP data doorbell 0x%04x", ms, v);
        }
    }
    /* FMT0, FMT1, RFS0: drain (the AP consumes these rings in Android too); other rings are left alone */
    static const uint16_t chs[3] = { SBM_CH_FMT0, SBM_CH_FMT1, SBM_CH_RFS0 };
    for (unsigned c = 0; c < 3; ++c) {
        int id = sbm_sbd_id(sbd, chs[c]);
        const uint8_t *f;
        int n;
        while (id >= 0 && (n = sbm_sbd_get(sbd, (unsigned)id, SBM_DL, &f, p)) != 0) {
            if (n < 0) {
                if (b->bad_rx++ < 5)
                    sbm_logf(p, "online: +%u ms: ch %u entry refused (%s), size word 0x%08x", ms, chs[c],
                             sbm_strerror(n), sbd->last_size_v);
                if (n == SBM_EIO)
                    break;          /* a broken index: leave this ring for now */
                continue;
            }
            struct sbm_sipc5 fr;
            const uint8_t *msg, *fb;
            uint32_t ml, fl;
            int whole = big_feed(b, &bigs[c], f, (uint32_t)n, sbd->r[id][SBM_DL].size, &fb, &fl);
            int ok = whole == 1 && sbm_sipc5_parse(fb, fl, &fr) == 0 && fr.ch == chs[c];
            int got = ok ? sbm_reasm_feed(&o_ra[c], &fr, &msg, &ml) : whole == 0 ? 0 : -1;
            if (got == 1 && c < 2) {
                struct sbm_ipc_msg m;
                b->fmt_rx++;
                if (sbm_ipc_parse(msg, ml, &m) == 0) {
                    sbm_cell_update(&b->cell, &m);
                    if (m.type == SBM_IPC_RESP && m.aseq >= SBM_QSEQ_FIRST && m.aseq <= SBM_QSEQ_LAST) {
                        uint32_t bit = 1u << (m.aseq & 31);
                        int seq = m.aseq;
                        if (!(o_open[seq >> 5] & bit)) {
                            uint16_t want = m.cmd == SBM_GEN_PHONE_RES && m.data_len >= 2
                                            ? (uint16_t)(m.data[0] << 8 | m.data[1]) : m.cmd;
                            seq = -1;
                            for (unsigned k = SBM_QSEQ_FIRST; k <= SBM_QSEQ_LAST && seq < 0; ++k)
                                if ((o_open[k >> 5] & (1u << (k & 31))) && o_qcmd[k] == want)
                                    seq = (int)k;
                            if (seq >= 0)
                                b->fmt_seq_fixed++;
                            bit = seq >= 0 ? 1u << (seq & 31) : 0;
                        }
                        if (seq < 0) {
                            b->fmt_unmatched++;
                        } else {                           /* an answer to one of our queries */
                            o_open[seq >> 5] &= ~bit;
                            if (m.cmd == SBM_GEN_PHONE_RES)
                                b->fmt_gen_res++;
                            else
                                b->fmt_resp++;
                        }
                    }
                    if (o_logged < ONLINE_LOG_MAX) {
                        char line[200];
                        sbm_frame_line(fb, fl, 1, line, sizeof(line));
                        sbm_logf(p, "FMT: +%u %s", ms, line);
                        o_logged++;
                    }
                } else {
                    b->bad_rx++;
                }
            } else if (got == 1) {
                int j = rfs_join(b, msg, ml, &msg, &ml);
                if (j < 0) {
                    if (b->bad_rx++ < 5)
                        sbm_logf(p, "online: +%u ms: RFS piece dropped (%u bytes; the join is reset)", ms, ml);
                    goto next;
                }
                if (j == 0)
                    goto next;
                b->rfs_rx++;
                log_rfs(p, b, msg, ml, o_rfs_logged < ONLINE_LOG_MAX);
                o_rfs_logged += o_rfs_logged < ONLINE_LOG_MAX;
                if (b->rfs_serve) {
                    int rl = sbm_rfs_handle(&o_rfs, msg, ml, o_reply, sizeof(o_reply));
                    if (rl > 0) {
                        int e = rfs_send(p, b, sbd, o_reply, (uint32_t)rl);
                        if (o_rfs_logged < ONLINE_LOG_MAX) {
                            sbm_logf(p, "RFS: +%u tx idx 0x%02x id %u len %u: %s %u (%s)", ms, o_reply[4], o_reply[5], rl,
                                     o_reply[4] == SBM_RFS_IDX_OPEN ? "errno" : "confirm",
                                     o_reply[4] == SBM_RFS_IDX_OPEN ? sbm_le32(o_reply + 10) : o_reply[6], e ? sbm_strerror(e) : "sent");
                            o_rfs_logged++;
                        }
                        if (e < 0) {
                            b->stopped_by = SBM_STOP_TIME;
                            g_online_ms = 0xFFFFFFFFu;
                            online_state(p, "the RFS reply could not be sent");
                            return e;
                        }
                        b->rfs_tx++;
                        b->rfs_tx_bytes += (uint32_t)rl;
                    }
                }
            } else if (got < 0) {
                /* the first bytes only for FMT (headers); for RFS only the 4-byte link header: never NV data */
                if (b->bad_rx++ < 5)
                    sbm_logf(p, "online: +%u ms: ch %u frame not parsed: %u bytes, size word 0x%08x, head %02x %02x %02x %02x"
                             " %02x %02x %02x %02x", ms, chs[c], n, sbd->last_size_v, f[0], f[1], f[2], f[3],
                             c < 2 && n > 4 ? f[4] : 0, c < 2 && n > 5 ? f[5] : 0, c < 2 && n > 6 ? f[6] : 0,
                             c < 2 && n > 7 ? f[7] : 0);
            }
        next:
            sbm_sbd_get_done(sbd, (unsigned)id, SBM_DL, p);
            uint32_t frames = b->fmt_rx + b->rfs_rx + b->bad_rx;
            if (ms - o_win_ms >= 60000) {
                o_win_ms = ms;
                o_win_frames = frames;
            }
            if (frames - o_win_frames >= ONLINE_FRAMES_PER_MIN) {
                b->stopped_by = SBM_STOP_FRAMES;
                g_online_ms = 0xFFFFFFFFu;
                return 1;
            }
        }
    }
    /* the guards: a SIM that is not absent, any registration */
    if (b->cell.have_sim) {
        b->sim_status = b->cell.sim_status;
        if (b->cell.sim_status != SBM_SIM_ABSENT && b->cell.sim_status != SBM_SIM_ERROR) {
            sbm_logf(p, "online: SIM status 0x%02x (not absent): stopping now, as planned", b->cell.sim_status);
            b->stopped_by = SBM_STOP_SIM;
            g_online_ms = 0xFFFFFFFFu;
            return 1;
        }
    }
    if ((b->cell.have_cs && (b->cell.cs.state == SBM_REG_HOME || b->cell.cs.state == SBM_REG_ROAMING)) ||
        (b->cell.have_ps && (b->cell.ps.state == SBM_REG_HOME || b->cell.ps.state == SBM_REG_ROAMING))) {
        sbm_logf(p, "online: the CP reports registration: stopping now");
        b->stopped_by = SBM_STOP_REG;
        g_online_ms = 0xFFFFFFFFu;
        return 1;
    }
    if (b->fmt_query && ms >= 3000 &&
        (ms == 3000 || (b->query_period_ms && (ms - 3000) % b->query_period_ms == 0))) {
        int e = fmt_queries(p, b, sbd, ms, ms == 3000);
        if (e < 0) {
            g_online_ms = 0xFFFFFFFFu;
            online_state(p, "a query could not be sent");
            return e;
        }
    }
    if (b->query_period_ms && !(ms % 60000)) {
        char line[160];
        sbm_cell_line_masked(&b->cell, line, sizeof(line));
        sbm_logf(p, "SOAK: +%u s: %s | FMT %u RFS %u queries %u answers %u", ms / 1000, line, b->fmt_rx, b->rfs_rx,
                 b->fmt_tx, b->fmt_resp + b->fmt_gen_res);
    }
    return 0;
}

static int online(const struct sbm_plat *p, struct sbm_boot *b, struct sbm_sbd *sbd, struct sbm_image *im)
{
    int r = online_enter(p, b, sbd, im);
    if (r < 0)
        return r;
    if (b->keep_running == SBM_KEEP_RUNNING)
        return 0;                            /* the session API takes over (sbm_modem_poll) */
    const uint32_t limit = b->hold_ms ? b->hold_ms : 20000;
    for (uint32_t ms = 1; ms <= limit; ++ms) {
        r = online_poll(p, b, sbd, ms);
        if (r != 0)
            return r < 0 ? r : 0;
        if (!(ms % 1000))
            feed(p);
        delay(p, 1000);
    }
    b->stopped_by = SBM_STOP_TIME;
    g_online_ms = 0xFFFFFFFFu;
    online_state(p, "end");
    return 0;
}

/* ---- the session API (Starbite OS): start, poll from the event loop, snapshot, recover, stop = park ----------- */

#define NONE 0xFFFFFFFFu
static uint32_t s_silent_since = NONE;  /* the first query sent after the CP last said anything (stall detection) */

static unsigned open_queries(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 8; ++i)
        for (uint32_t v = o_open[i]; v; v &= v - 1)
            ++n;
    return n;
}

static void went_down(struct sbm_modem *m, uint8_t state, int32_t err)
{
    if (m->up || m->b.inited)
        m->b.parked = sbm_cp_park(m->p) == 0;
    m->up = 0;
    m->state = state;
    m->last_err = err;
    m->down_ms = m->total_ms;
}

static int start_core(struct sbm_modem *m)
{
    struct sbm_image *im = m->im;
    if (im->nv_pristine)                     /* EFS's data as read, whatever the CP wrote last time */
        for (unsigned k = 0; k < im->plan.n; ++k)
            if (im->plan.l[k].src == SBM_SRC_NV && im->data[k])
                sbm_memcpy((uint8_t *)(uintptr_t)im->data[k], im->nv_pristine, im->plan.l[k].size);
    sbm_memset(&m->b, 0, sizeof(m->b));
    m->b.stop = SBM_BOOT_ONLINE;
    m->b.rfs_serve = 1;
    m->b.keep_running = SBM_KEEP_RUNNING;
    m->ms = 0;
    m->last_rx_ms = NONE;
    m->rx_seen = 0;
    s_silent_since = NONE;
    int r = sbm_boot_run(m->p, im, &m->b);
    m->up = r == 0 && m->b.init_end_sent && !m->b.parked;
    if (!m->up) {
        went_down(m, SBM_MS_FAILED, r < 0 ? r : SBM_EIO);   /* sbm_boot_run parked it; parking again is harmless */
        return m->last_err;
    }
    m->state = SBM_MS_ONLINE;
    m->last_err = 0;
    return 0;
}

int sbm_modem_start(struct sbm_modem *m, const struct sbm_plat *p, struct sbm_image *im)
{
    if (!m || !p || !im)
        return SBM_EINVAL;
    if (m->up)
        return 0;
    m->p = p;
    m->im = im;
    m->starts++;
    return start_core(m);
}

int sbm_modem_poll(struct sbm_modem *m, uint32_t elapsed_ms)
{
    if (!m->up)
        return SBM_EPERM;
    elapsed_ms = elapsed_ms ? elapsed_ms : 1;
    m->ms += elapsed_ms;
    m->total_ms += elapsed_ms;
    uint32_t msgs = m->b.cell.n_msgs;
    int r = online_poll(m->p, &m->b, o_sbd, m->ms);
    uint32_t rx = m->b.fmt_rx + m->b.rfs_rx + m->b.bad_rx;
    if (rx != m->rx_seen) {
        m->rx_seen = rx;
        m->last_rx_ms = m->ms;
        s_silent_since = NONE;
    }
    if (r != 0) {                            /* a guard, a crash, the flood guard: park now */
        int crash = m->b.stopped_by == SBM_STOP_CRASH || r < 0;
        if (crash)
            m->crashes++;
        went_down(m, crash ? SBM_MS_CRASHED : SBM_MS_GUARD, crash ? (r < 0 ? r : SBM_EIO) : SBM_EPERM);
        return m->last_err;
    }
    if (s_silent_since != NONE && m->ms - s_silent_since >= SBM_STALL_MS) {
        sbm_logf(m->p, "session: +%u ms: no word from the CP for %u ms after a query (%u open): stalled, parking",
                 m->ms, m->ms - s_silent_since, open_queries());
        m->stalls++;
        went_down(m, SBM_MS_STALLED, SBM_ETIME);
        return SBM_ETIME;
    }
    return m->b.cell.n_msgs != msgs;
}

int sbm_modem_query(struct sbm_modem *m)
{
    if (!m->up)
        return SBM_EPERM;
    int r = fmt_queries(m->p, &m->b, o_sbd, m->ms, 0);
    if (r == 0 && s_silent_since == NONE)
        s_silent_since = m->ms;
    return r;
}

void sbm_modem_status(const struct sbm_modem *m, char *out, size_t cap)
{
    if (!m->up) {
        sbm_strlcpy(out, m->state == SBM_MS_STALLED ? "modem off (stalled, parked)"
                         : m->state == SBM_MS_CRASHED ? "modem off (crashed, parked)"
                         : m->state == SBM_MS_GUARD ? "modem off (a guard parked it)"
                         : m->state == SBM_MS_FAILED ? "modem off (the start failed, parked)"
                         : m->b.parked ? "modem off (parked)" : "modem off", cap);
        return;
    }
    sbm_cell_line_masked(&m->b.cell, out, cap);
}

void sbm_modem_snapshot(struct sbm_modem *m, struct sbm_modem_snap *s)
{
    const struct sbm_cell_state *c = &m->b.cell;
    sbm_memset(s, 0, sizeof(*s));
    s->state = m->state;
    s->sim = s->cs = s->ps = s->bars = s->rssi_raw = 0xFF;
    if (m->up) {
        if (c->have_sim)
            s->sim = c->sim_status;
        if (c->have_cs)
            s->cs = c->cs.state;
        if (c->have_ps)
            s->ps = c->ps.state;
        s->rat = c->have_cs && c->cs.act ? c->cs.act : c->have_ps ? c->ps.act : 0;
        if (c->have_bars)
            s->bars = c->bars;
        if (c->have_rssi) {
            s->rssi_raw = c->rssi_raw[0];
            s->rssi_dbm = (int16_t)-(int16_t)c->rssi_raw[0];
        }
        int served = (c->have_cs && (c->cs.state == SBM_REG_HOME || c->cs.state == SBM_REG_ROAMING)) ||
                     (c->have_ps && (c->ps.state == SBM_REG_HOME || c->ps.state == SBM_REG_ROAMING));
        s->emergency_only = !served && ((c->have_cs && c->cs.state == SBM_REG_EMERGENCY) ||
                                         (c->have_ps && c->ps.state == SBM_REG_EMERGENCY));
        if (c->have_plmn) {
            s->have_plmn = 1;
            sbm_memcpy(s->plmn, c->plmn, sizeof(s->plmn));
        }
        s->have_cell = (c->have_cs && c->cs.have_loc) || (c->have_ps && c->ps.have_loc);
        s->queries = m->b.fmt_tx;
        s->answers = m->b.fmt_resp + m->b.fmt_gen_res;
        s->open = open_queries();
    }
    s->starts = m->starts;
    s->restarts = m->restarts;
    s->last_err = m->last_err;
    sbm_modem_status(m, s->line, sizeof(s->line));
    /* gen: only what a screen shows (not the clocks, not the counters) */
    struct sbm_modem_snap *l = &m->last_snap;
    if (l->state != s->state || l->sim != s->sim || l->cs != s->cs || l->ps != s->ps || l->rat != s->rat ||
        l->bars != s->bars || l->rssi_raw != s->rssi_raw || l->emergency_only != s->emergency_only ||
        l->have_plmn != s->have_plmn || sbm_memcmp(l->plmn, s->plmn, sizeof(s->plmn)) || l->have_cell != s->have_cell ||
        l->last_err != s->last_err || m->gen == 0) {
        m->gen++;
        *l = *s;
    }
    s->gen = m->gen;
    s->up_ms = m->up ? m->ms : 0;
    s->last_rx_ms = m->up && m->last_rx_ms != NONE ? m->ms - m->last_rx_ms : NONE;
}

int sbm_modem_recover(struct sbm_modem *m, uint32_t elapsed_ms)
{
    if (m->up)
        return 0;
    if (!m->p || (m->state != SBM_MS_STALLED && m->state != SBM_MS_CRASHED))
        return SBM_EPERM;
    m->total_ms += elapsed_ms;
    if (m->total_ms - m->down_ms < SBM_RECOVER_GAP_MS)
        return 1;
    unsigned recent = 0, slot = 0;
    for (unsigned i = 0; i < SBM_RECOVER_MAX; ++i) {
        if (m->restart_at[i] && m->total_ms - m->restart_at[i] < SBM_RECOVER_WINDOW_MS)
            ++recent;
        if (m->restart_at[i] < m->restart_at[slot])
            slot = i;
    }
    if (recent >= SBM_RECOVER_MAX) {
        sbm_logf(m->p, "session: %u restarts in the last %u min: staying off", recent, SBM_RECOVER_WINDOW_MS / 60000);
        return SBM_EPERM;
    }
    m->restart_at[slot] = m->total_ms ? m->total_ms : 1;
    m->restarts++;
    sbm_logf(m->p, "session: restart %u (after %s)", m->restarts, m->state == SBM_MS_STALLED ? "a stall" : "a crash");
    return start_core(m);
}

int sbm_modem_stop(struct sbm_modem *m)
{
    int r = m->p && (m->up || m->b.inited) ? sbm_cp_park(m->p) : 0;
    m->up = 0;
    m->b.parked = r == 0;
    m->state = SBM_MS_OFF;
    return r;
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
    if (b->stop == SBM_BOOT_ONLINE)
        return online(p, b, &sbd, im);
    if (b->hold_ms)
        return hold(p, b, &sbd);
    return 0;
}
