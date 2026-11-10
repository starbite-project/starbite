/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: the fake CP (see fakecp.h). Payload shapes copy the ones seen on this phone's FMT traffic
 * (docs/MODEM.md section 3.4), with test values only. */
#include <string.h>
#include "fakecp.h"

int fakecp_attach(struct fakecp *fc, uint8_t *ipc, uint32_t size, enum fake_scenario sc)
{
    memset(fc, 0, sizeof(*fc));
    fc->sc = sc;
    return sbm_sbd_attach(&fc->sbd, ipc, size);
}

int fakecp_raw(struct fakecp *fc, uint8_t ch, const uint8_t *b, uint32_t n)
{
    return sbm_sbd_put(&fc->sbd, ch, SBM_DL, b, n, 0);
}

int fakecp_send(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint16_t cmd, uint8_t type, const uint8_t *d, uint16_t n)
{
    uint8_t f[4096];
    uint32_t l = 4 + SBM_IPC_HDR + n;
    if (l > sizeof(f))
        return SBM_ENOSPC;
    f[0] = SBM_SIPC5_START;
    f[1] = ch;
    sbm_put16(f + 2, (uint16_t)l);
    sbm_put16(f + 4, (uint16_t)(SBM_IPC_HDR + n));
    f[6] = fc->mseq++;
    f[7] = aseq;
    f[8] = (uint8_t)(cmd >> 8);
    f[9] = (uint8_t)cmd;
    f[10] = type;
    if (n)
        memcpy(f + 11, d, n);
    if (type == SBM_IPC_NOTI)
        fc->n_noti++;
    else
        fc->n_resp++;
    return fakecp_raw(fc, ch, f, l);
}

int fakecp_send_multi(struct fakecp *fc, uint8_t ch, uint8_t id, uint16_t cmd, uint8_t type, const uint8_t *d,
                      uint16_t n, uint32_t frag)
{
    uint8_t msg[8192], f[4096];
    uint32_t ml = SBM_IPC_HDR + n;
    if (ml > sizeof(msg) || frag == 0 || frag + 5 > sizeof(f))
        return SBM_ENOSPC;
    sbm_put16(msg, (uint16_t)ml);
    msg[2] = fc->mseq++;
    msg[3] = 0;
    msg[4] = (uint8_t)(cmd >> 8);
    msg[5] = (uint8_t)cmd;
    msg[6] = type;
    memcpy(msg + SBM_IPC_HDR, d, n);
    for (uint32_t o = 0; o < ml; o += frag) {
        uint32_t k = ml - o < frag ? ml - o : frag;
        int l = sbm_sipc5_build_multi(f, sizeof(f), ch, id, o + k < ml, msg + o, k);
        if (l < 0)
            return l;
        int e = fakecp_raw(fc, ch, f, (uint32_t)l);
        if (e < 0)
            return e;
    }
    return 0;
}

static void regist(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint8_t type, uint8_t domain)
{
    /* act, domain, state, edge, lac(2), cid(4), reject, then 8 bytes this firmware adds (zeros here) */
    uint8_t d[19] = { 0 };
    switch (fc->sc) {
    case FAKE_NOSIM:
        d[0] = SBM_ACT_GSM;
        d[2] = SBM_REG_EMERGENCY;
        break;
    case FAKE_HOME:
        d[0] = SBM_ACT_LTE;
        d[2] = SBM_REG_HOME;
        break;
    case FAKE_SEARCH:
        d[0] = SBM_ACT_GSM;
        d[2] = SBM_REG_SEARCHING;
        break;
    }
    d[1] = domain;
    d[4] = 0x12; d[5] = 0x34;                       /* test LAC / cell id */
    d[6] = 0x01; d[7] = 0x02; d[8] = 0x03; d[9] = 0x04;
    fakecp_send(fc, ch, aseq, SBM_NET_REGIST, type, d, sizeof(d));
}

static void pin_status(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint8_t type)
{
    uint8_t d[2] = { fc->sc == FAKE_NOSIM ? SBM_SIM_ABSENT : SBM_SIM_INIT_COMPLETE, 0 };
    fakecp_send(fc, ch, aseq, SBM_SEC_PIN_STATUS, type, d, 2);
}

void fakecp_boot_notis(struct fakecp *fc)
{
    uint8_t up = 0x53, st = 0x02, icc = fc->sc == FAKE_NOSIM ? 0 : 2;
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_PWR_PHONE_PWR_UP, SBM_IPC_NOTI, &up, 1);
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_SEC_SIM_ICC_TYPE, SBM_IPC_NOTI, &icc, 1);
    pin_status(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI);
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_PWR_PHONE_STATE, SBM_IPC_NOTI, &st, 1);
    regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_CS);
    regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_PS);
}

void fakecp_set_scenario(struct fakecp *fc, enum fake_scenario sc)
{
    fc->sc = sc;
    pin_status(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI);
    regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_CS);
    regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_PS);
}

static void gen_res(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint16_t cmd, uint8_t type, uint16_t code)
{
    uint8_t d[5] = { (uint8_t)(cmd >> 8), (uint8_t)cmd, type, (uint8_t)code, (uint8_t)(code >> 8) };
    fakecp_send(fc, ch, aseq, SBM_GEN_PHONE_RES, SBM_IPC_RESP, d, 5);
}

static void answer(struct fakecp *fc, uint8_t ch, const struct sbm_ipc_msg *m)
{
    fc->n_req++;
    if ((m->cmd >> 8) == SBM_G_CALL)
        fc->n_call++;
    if (!sbm_ipc_allowed(m->cmd, m->type))
        fc->n_forbidden++;
    if (m->type != SBM_IPC_GET) {
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x000a);
        return;
    }
    switch (m->cmd) {
    case SBM_SEC_PIN_STATUS:
        pin_status(fc, ch, m->mseq, SBM_IPC_RESP);
        break;
    case SBM_PWR_PHONE_STATE: {
        uint8_t st = 0x02;
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, &st, 1);
        break;
    }
    case SBM_SEC_SIM_ICC_TYPE: {
        uint8_t icc = fc->sc == FAKE_NOSIM ? 0 : 2;
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, &icc, 1);
        break;
    }
    case SBM_NET_REGIST: {
        uint8_t dom = m->data_len >= 2 ? m->data[1] : SBM_DOMAIN_CS;
        regist(fc, ch, m->mseq, SBM_IPC_RESP, dom);
        break;
    }
    case SBM_NET_SERVING_NETWORK: {
        /* u8, state, u8, plmn[6] ASCII ('#' padding), lac, then zeros to this firmware's 26 bytes */
        uint8_t d[26] = { 0x02, 0x01, 0xff, '#', '#', '#', '#', '#', '#' };
        if (fc->sc == FAKE_HOME || fc->nosim_plmn)
            memcpy(d + 3, "00101#", 6);
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, d, sizeof(d));
        break;
    }
    case SBM_NET_PLMN_SEL: {
        uint8_t mode = 0x02;
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, &mode, 1);
        break;
    }
    case SBM_DISP_ICON_INFO: {
        uint8_t d[12] = { 0x01, fc->sc == FAKE_HOME ? 3 : 0 };
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, d, sizeof(d));
        break;
    }
    case SBM_DISP_RSSI_INFO: {
        uint8_t d[22] = { 0x3e, 0x19, 0xff, 0x63 };
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, d, sizeof(d));
        break;
    }
    default:
        fc->n_unknown++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x000a);   /* the error seen for an unsupported GET */
        break;
    }
}

int fakecp_step(struct fakecp *fc)
{
    int handled = 0;
    static const uint8_t fmt[] = { SBM_CH_FMT0, SBM_CH_FMT1 };
    for (unsigned k = 0; k < 2; ++k) {
        int id = sbm_sbd_id(&fc->sbd, fmt[k]);
        if (id < 0)
            continue;
        for (;;) {
            const uint8_t *f;
            int n = sbm_sbd_get(&fc->sbd, (unsigned)id, SBM_UL, &f, 0);
            if (n == 0)
                break;
            if (n < 0) {
                fc->n_bad++;
                continue;
            }
            struct sbm_sipc5 fr;
            struct sbm_ipc_msg m;
            if (sbm_sipc5_parse(f, (uint32_t)n, &fr) == 0 && fr.ch == fmt[k] && !fr.multi &&
                sbm_ipc_parse(fr.payload, fr.payload_len, &m) == 0) {
                fc->last_ch = fr.ch;
                answer(fc, fr.ch, &m);
                handled++;
            } else {
                fc->n_bad++;
            }
            sbm_sbd_get_done(&fc->sbd, (unsigned)id, SBM_UL, 0);
        }
    }
    return handled;
}

int fakeregs_probe32(void *ctx, uint64_t pa, uint32_t *v)
{
    struct fakeregs *r = ctx;
    if (r->ntrace < sizeof(r->trace) / sizeof(r->trace[0]))
        r->trace[r->ntrace++] = pa;
    *v = 0;
    if (pa & 3)
        return 0;
    if (pa >= SBM_PMU_PA && pa < SBM_PMU_PA + sizeof(r->pmu)) {
        *v = r->pmu[(pa - SBM_PMU_PA) / 4];
        return 1;
    }
    if (pa >= SBM_MBOX_PA && pa < SBM_MBOX_PA + SBM_MBOX_SIZE) {
        if (r->mbox_faults)
            return 0;
        *v = r->mbox[(pa - SBM_MBOX_PA) / 4];
        return 1;
    }
    if (pa >= SBM_MODEM_IF_PA && pa < SBM_MODEM_IF_PA + SBM_MODEM_IF_SIZE) {
        r->shm_reads++;
        if (!r->shm_head)
            return 0;
        uint64_t o = pa - SBM_MODEM_IF_PA;
        for (unsigned i = 0; i < r->ncell; ++i)
            if (r->cell_pa[i] == pa) {
                *v = r->cell_v[i];
                return 1;
            }
        if (r->raw_len && o >= 0x400) {
            uint8_t b[4] = { 0 };
            for (unsigned k = 0; k < 4; ++k)
                b[k] = o + k < r->raw_len ? fake_radio_byte(r, o + k) : 0;
            *v = sbm_le32(b);
            return 1;
        }
        if (o + 4 <= 0x400) {
            *v = sbm_le32(r->shm_head + o);
            return 1;
        }
        if (o >= SBM_DT_IPC_OFF && o + 4 <= SBM_DT_IPC_OFF + 8) {
            *v = sbm_le32(r->ipc_head + (o - SBM_DT_IPC_OFF));
            return 1;
        }
        return 0;
    }
    return 0;
}

uint8_t fake_radio_byte(const struct fakeregs *r, uint64_t off)
{
    if (off < 0x400 && r->shm_head)
        return r->shm_head[off];
    uint32_t x = (uint32_t)off * 2654435761U;
    return (uint8_t)(x >> 24 ^ x >> 13);
}

int fakeregs_radio_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    struct fakeregs *r = ctx;
    if (off + len > 52428800ULL)
        return -1;
    for (uint32_t i = 0; i < len; ++i)
        ((uint8_t *)buf)[i] = fake_radio_byte(r, off + i);
    return 0;
}

int fakeregs_poke32(void *ctx, uint64_t pa, uint32_t v)
{
    struct fakeregs *r = ctx;
    if (r->npoke < 8)
        r->poke_pa[r->npoke] = pa;
    r->npoke++;
    if (r->store_mode == STORE_FAULTS)
        return 0;
    if (r->store_mode == STORE_IGNORED)
        return 1;
    for (unsigned i = 0; i < r->ncell; ++i)
        if (r->cell_pa[i] == pa) {
            r->cell_v[i] = v;
            return 1;
        }
    if (r->ncell < 4) {
        r->cell_pa[r->ncell] = pa;
        r->cell_v[r->ncell++] = v;
    }
    return 1;
}

uint64_t fakeregs_smc(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct fakeregs *r = ctx;
    if (r->nsmc < 16) {
        r->smc_args[r->nsmc][0] = id;
        r->smc_args[r->nsmc][1] = a1;
        r->smc_args[r->nsmc][2] = a2;
        r->smc_args[r->nsmc][3] = a3;
    }
    r->nsmc++;
    if (r->smc_err)
        return r->smc_err;
    if (id != SBM_SMC_CP || a1 != SBM_SMC_READ_CTRL || a2 > 1 || a3 > 1)
        return 0xFFFF;
    uint32_t v = r->ctrl[a3];
    return (uint64_t)((a2 ? v >> 16 : v & 0xFFFF) << 16);
}
