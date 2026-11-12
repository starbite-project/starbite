/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: the fake CP (see fakecp.h). Payload shapes copy the ones seen on this phone's FMT traffic
 * (docs/MODEM.md section 3.4), with test values only. */
#include <stdio.h>
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
    case FAKE_SIM:
        d[0] = fc->radio_normal ? SBM_ACT_LTE : SBM_ACT_GSM;
        d[2] = fc->radio_normal && !fc->pin_lock && (!fc->need_attach_apn || fc->n_attach_apn) ? SBM_REG_HOME
             : fc->radio_normal && fc->pin_lock ? SBM_REG_EMERGENCY : SBM_REG_NONE;
        break;
    }
    d[1] = domain;
    d[4] = 0x12; d[5] = 0x34;                       /* test LAC / cell id */
    d[6] = 0x01; d[7] = 0x02; d[8] = 0x03; d[9] = 0x04;
    fakecp_send(fc, ch, aseq, SBM_NET_REGIST, type, d, sizeof(d));
}

static void pin_status(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint8_t type)
{
    uint8_t d[2] = { fc->sc == FAKE_NOSIM ? SBM_SIM_ABSENT : fc->sc == FAKE_SIM && fc->pin_lock ? SBM_SIM_LOCK_SC
                                                                                                : SBM_SIM_INIT_COMPLETE,
                     (uint8_t)(fc->sc == FAKE_SIM && fc->pin_lock ? 0x01 : 0) };
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

#ifdef SBM_ATTENDED_TX
/* ---- calls and SMS: the CP's side as libsec-ril sees it (docs/MODEM.md 7.11) ---------------------------------- */
static void call_status(struct fakecp *fc, int state, int cls, int cause)
{
    uint8_t d[6] = { 0x00, 0x01, (uint8_t)fc->call.id, (uint8_t)state, (uint8_t)cls, (uint8_t)cause };
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_CALL_STATUS, SBM_IPC_NOTI, d, 6);
}

static void call_list_resp(struct fakecp *fc, uint8_t ch, uint8_t aseq)
{
    uint8_t d[64] = { 0 };
    unsigned o = 1;
    if (fc->call.used) {
        unsigned L = (unsigned)strlen(fc->call.number);
        d[0] = 1;
        d[o + 0] = 0x00, d[o + 1] = 0x01, d[o + 2] = (uint8_t)fc->call.id;
        d[o + 3] = fc->call.outgoing ? 1 : 2;
        d[o + 4] = (uint8_t)fc->call.state;
        d[o + 5] = 0;
        d[o + 6] = (uint8_t)L;
        d[o + 7] = fc->call.number[0] == '+' ? 0x11 : 0x21;
        memcpy(d + o + 8, fc->call.number, L);
        o += 8 + L;
    }
    fakecp_send(fc, ch, aseq, SBM_CALL_LIST, SBM_IPC_RESP, d, (uint16_t)o);
}

void fakecp_ring(struct fakecp *fc, const char *number)
{
    memset(&fc->call, 0, sizeof(fc->call));
    fc->call.used = 1;
    fc->call.id = 2;
    fc->call.state = SBM_CALL_INCOMING_ST;
    snprintf(fc->call.number, sizeof(fc->call.number), "%s", number);
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_CALL_INCOMING, SBM_IPC_NOTI, 0, 0);
}

void fakecp_sms_deliver(struct fakecp *fc, const uint8_t *tpdu, uint32_t n)
{
    uint8_t d[220];
    static const uint8_t sca[] = { 0x07, 0x91, 0x00, 0x10, 0x10, 0x00, 0x00, 0xF0 };   /* a test SMSC */
    fc->tpid++;
    d[0] = 2, d[1] = 1, d[2] = 0, d[3] = 0, d[4] = fc->tpid;
    d[5] = (uint8_t)(sizeof(sca) + n);
    memcpy(d + 6, sca, sizeof(sca));
    memcpy(d + 6 + sizeof(sca), tpdu, n);
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_SMS_INCOMING, SBM_IPC_NOTI, d, (uint16_t)(6 + sizeof(sca) + n));
}

/* a delivery report (0x0402 with data[1] = 2) for the reference mr with TP-ST st */
void fakecp_sms_report(struct fakecp *fc, uint8_t mr, uint8_t st)
{
    static const uint8_t t[] = { 0x06, 0x00, 0x0C, 0x91, 0x53, 0x48, 0x40, 0x89, 0x67, 0x45,
                                 0x52, 0x11, 0x21, 0x01, 0x02, 0x03, 0x00, 0x52, 0x11, 0x21, 0x01, 0x02, 0x04, 0x00, 0x00 };
    uint8_t tp[sizeof(t)];
    memcpy(tp, t, sizeof(t));
    tp[1] = mr;
    tp[sizeof(t) - 1] = st;
    uint8_t d[64];
    fc->tpid++;
    d[0] = 2, d[1] = 2, d[2] = 0, d[3] = 0, d[4] = fc->tpid;
    d[5] = (uint8_t)(1 + sizeof(tp));
    d[6] = 0;                               /* no SMSC */
    memcpy(d + 7, tp, sizeof(tp));
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_SMS_INCOMING, SBM_IPC_NOTI, d, (uint16_t)(7 + sizeof(tp)));
}

void fakecp_ecc(struct fakecp *fc, const char *const *numbers, unsigned n)
{
    uint8_t d[96] = { 0 };
    unsigned o = 7;
    d[6] = (uint8_t)n;
    for (unsigned i = 0; i < n; ++i) {
        unsigned L = (unsigned)strlen(numbers[i]);
        d[o] = 0x01, d[o + 1] = (uint8_t)L;
        memcpy(d + o + 2, numbers[i], L);
        o += 2 + L;
    }
    fakecp_send(fc, SBM_CH_FMT0, 0, SBM_CALL_ECC_LIST, SBM_IPC_NOTI, d, (uint16_t)o);
}

/* the call moves on a few steps at a time: dialing -> alerting -> active (the far end answers) */
static void fake_call_tick(struct fakecp *fc)
{
    if (!fc->call.used || !fc->call.outgoing)
        return;
    fc->call.steps++;
    if (fc->call.state == SBM_CALL_DIALING && fc->call.steps == 3) {
        fc->call.state = SBM_CALL_ALERTING;
        call_status(fc, 2, 0, 0);
    } else if (fc->call.state == SBM_CALL_ALERTING && fc->call.steps == 6) {
        fc->call.state = SBM_CALL_ACTIVE;
        call_status(fc, 3, 0, 0);
    }
}

static int fake_comms(struct fakecp *fc, uint8_t ch, const struct sbm_ipc_msg *m)
{
    int known = m->cmd == SBM_CALL_DIAL || m->cmd == SBM_CALL_RELEASE || m->cmd == SBM_CALL_ANSWER ||
                m->cmd == SBM_CALL_LIST || m->cmd == SBM_CALL_DTMF || m->cmd == SBM_SS_MANAGE_CALL ||
                m->cmd == SBM_SMS_SEND || m->cmd == SBM_SMS_ACK || m->cmd == SBM_SMS_READY || m->cmd == SBM_SMS_SMSC ||
                m->cmd == SBM_SND_PATH || m->cmd == SBM_SND_VOLUME || m->cmd == SBM_SND_MUTE;
    if (!known)
        return 0;
    if (!sbm_ipc_attended_allowed(m->cmd, m->type, m->data, m->data_len)) {
        fc->n_forbidden++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x000a);
        return 1;
    }
    switch (m->cmd) {
    case SBM_CALL_DIAL:
        fc->n_dial++;
        memset(&fc->call, 0, sizeof(fc->call));
        fc->call.used = 1, fc->call.id = 1, fc->call.outgoing = 1, fc->call.state = SBM_CALL_DIALING;
        memcpy(fc->call.number, m->data + 5, m->data[3] < 23 ? m->data[3] : 23);
        snprintf(fc->dialled, sizeof(fc->dialled), "%s", fc->call.number);
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8001);
        call_status(fc, 1, 0, 0);
        break;
    case SBM_CALL_ANSWER:
        fc->n_answer++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        if (fc->call.used && fc->call.state == SBM_CALL_INCOMING_ST) {
            fc->call.state = SBM_CALL_ACTIVE;
            call_status(fc, 3, 0, 0);
        }
        break;
    case SBM_CALL_RELEASE:
    case SBM_SS_MANAGE_CALL:
        fc->n_release++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        if (fc->call.used) {
            call_status(fc, 4, 0, 5);                /* released, normal clearing */
            fc->call.used = 0;
        }
        break;
    case SBM_CALL_LIST:
        fc->n_list++;
        call_list_resp(fc, ch, m->mseq);
        break;
    case SBM_CALL_DTMF:
        fc->n_dtmf++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        break;
    case SBM_SND_PATH:
    case SBM_SND_VOLUME:
    case SBM_SND_MUTE:
        fc->n_snd++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        break;
    case SBM_SMS_READY:
        fc->n_sms_ready++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        break;
    case SBM_SMS_SMSC: {
        static const uint8_t smsc[] = { 0x07, 0x91, 0x00, 0x10, 0x10, 0x00, 0x00, 0xF0 };
        fc->n_smsc++;
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, smsc, sizeof(smsc));
        break;
    }
    case SBM_SMS_SEND: {
        unsigned k = fc->n_sms_sent < 4 ? fc->n_sms_sent : 3;
        unsigned sca = m->data[4];
        fc->sms_len[k] = (uint16_t)(m->data_len - 5 - sca);
        memcpy(fc->sms_pdu[k], m->data + 5 + sca, fc->sms_len[k] < 200 ? fc->sms_len[k] : 200);
        fc->n_sms_sent++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        uint8_t r[5] = { 2, 0, 0, (uint8_t)(0x40 + fc->n_sms_sent), 0 };
        if (fc->sms_fail) {
            r[1] = 0x2a, r[2] = 0x80;                /* 0x802a: RP cause 42, congestion */
            fc->sms_fail = 0;
        }
        fakecp_send(fc, ch, m->mseq, m->cmd, SBM_IPC_RESP, r, 5);
        break;
    }
    case SBM_SMS_ACK:
        if (m->data[3] == fc->tpid)
            fc->n_ack++, fc->acked_tpid = m->data[3];
        else
            fc->n_ack_bad++;
        break;
    default:
        break;
    }
    return 1;
}
#endif

static void answer(struct fakecp *fc, uint8_t ch, const struct sbm_ipc_msg *m)
{
    fc->n_req++;
    if ((m->cmd >> 8) == SBM_G_CALL)
        fc->n_call++;
    if (m->cmd == SBM_PWR_PHONE_STATE && m->type == SBM_IPC_EXEC && m->data_len == 2 &&
        ((m->data[0] == 0x02 && m->data[1] == 0x02) || (m->data[0] == 0x01 && m->data[1] == 0x00))) {
        fc->n_radio_exec++;
        if (fc->sc == FAKE_SIM)
            fc->radio_normal = m->data[0] == 0x02;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_CS);
        regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_PS);
        return;
    }
#ifdef SBM_ATTENDED_TX
    if (fake_comms(fc, ch, m))
        return;
    if (m->cmd == SBM_NET_PLMN_SEL && m->type == SBM_IPC_SET &&
        sbm_ipc_attended_allowed(m->cmd, m->type, m->data, m->data_len)) {
        fc->n_net_auto++;
        gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        return;
    }
    if ((m->cmd >> 8) == SBM_G_GPRS && sbm_ipc_attended_allowed(m->cmd, m->type, m->data, m->data_len)) {
        fc->n_gprs++;
        if (m->cmd == SBM_GPRS_LTE_ATTACH) {
            fc->n_attach_apn++;
            gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
            regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_CS);
            regist(fc, SBM_CH_FMT0, 0, SBM_IPC_NOTI, SBM_DOMAIN_PS);
        } else if (m->cmd == SBM_GPRS_PS) {
            fc->n_ps_attach++;
            gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        } else if (m->cmd == SBM_GPRS_DEFINE_PDP) {
            fc->n_define++;
            memcpy(fc->apn, m->data + 3, 100);
            fc->apn[100] = 0;
            gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        } else if (m->cmd == SBM_GPRS_PDP_CONTEXT) {
            if (m->data_len == SBM_GPRS_ACT_LEN && fc->act_refuse_short) {
                gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x000a);
                return;
            }
            gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
            if (m->data[0] == 1 && fc->n_define) {
                uint8_t ipc[160] = { 1, 0x19, 0x20 };   /* cid 1, flags IPv4 | gw | mask | MTU (0x2019) */
                const uint8_t ip[4] = { 10, 11, 12, 13 }, d1[4] = { 10, 0, 0, 53 }, gw[4] = { 10, 11, 12, 1 };
                memcpy(ipc + 3, ip, 4);
                memcpy(ipc + 7, d1, 4);
                memcpy(ipc + 15, gw, 4);
                ipc[152] = 0xdc; ipc[153] = 0x05;       /* MTU 1500 */
                fc->pdp_up = 1;
                fc->n_act++;
                fakecp_send(fc, SBM_CH_FMT0, 0, SBM_GPRS_IP_CONFIG, SBM_IPC_NOTI, ipc, sizeof(ipc));
                uint8_t st[9] = { 1, 1, 0 };
                fakecp_send(fc, SBM_CH_FMT0, 0, SBM_GPRS_CALL_STATUS, SBM_IPC_NOTI, st, sizeof(st));
            } else if (m->data[0] == 0) {
                fc->pdp_up = 0;
                fc->n_deact++;
                uint8_t st[9] = { 1, 0, 0 };
                fakecp_send(fc, SBM_CH_FMT0, 0, SBM_GPRS_CALL_STATUS, SBM_IPC_NOTI, st, sizeof(st));
            }
        } else {
            gen_res(fc, ch, m->mseq, m->cmd, m->type, 0x8000);
        }
        return;
    }
#endif
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
        regist(fc, ch, fc->regist_seq_quirk && dom == SBM_DOMAIN_CS ? (uint8_t)(m->mseq + 1) : m->mseq, SBM_IPC_RESP,
               dom);
        break;
    }
    case SBM_NET_SERVING_NETWORK: {
        /* u8, state, u8, plmn[6] ASCII ('#' padding), lac, then zeros to this firmware's 26 bytes */
        uint8_t d[26] = { 0x02, 0x01, 0xff, '#', '#', '#', '#', '#', '#' };
        if (fc->sc == FAKE_HOME || fc->nosim_plmn || (fc->sc == FAKE_SIM && fc->radio_normal))
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
        uint8_t d[12] = { 0x01, fc->sc == FAKE_HOME || (fc->sc == FAKE_SIM && fc->radio_normal && !fc->pin_lock) ? 3 : 0 };
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
#ifdef SBM_ATTENDED_TX
    fake_call_tick(fc);
#endif
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

/* ---- the fake network behind the PS rings ------------------------------------------------------------------ */

static uint16_t f_csum_add(uint32_t s, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i + 1 < n; i += 2)
        s += (uint32_t)b[i] << 8 | b[i + 1];
    if (n & 1)
        s += (uint32_t)b[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)s;
}
static void f_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void f_put32(uint8_t *p, uint32_t v) { f_put16(p, (uint16_t)(v >> 16)); f_put16(p + 2, (uint16_t)v); }
static uint16_t f_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t f_get32(const uint8_t *p) { return (uint32_t)f_get16(p) << 16 | f_get16(p + 2); }

/* an IPv4 packet from src to the phone (10.11.12.13), checksums filled */
static uint32_t f_ip(uint8_t *pk, const uint8_t src[4], uint8_t proto, uint32_t l4len)
{
    static const uint8_t me[4] = { 10, 11, 12, 13 };
    uint32_t tot = 20 + l4len;
    memset(pk, 0, 20);
    pk[0] = 0x45;
    f_put16(pk + 2, (uint16_t)tot);
    f_put16(pk + 6, 0x4000);
    pk[8] = 60;
    pk[9] = proto;
    memcpy(pk + 12, src, 4);
    memcpy(pk + 16, me, 4);
    f_put16(pk + 10, (uint16_t)~f_csum_add(0, pk, 20));
    uint8_t ph[12];
    memcpy(ph, src, 4);
    memcpy(ph + 4, me, 4);
    ph[8] = 0;
    ph[9] = proto;
    f_put16(ph + 10, (uint16_t)l4len);
    uint8_t *l4 = pk + 20;
    unsigned co = proto == 6 ? 16 : 6;
    f_put16(l4 + co, 0);
    uint16_t c = (uint16_t)~f_csum_add(f_csum_add(0, ph, 12), l4, l4len);
    f_put16(l4 + co, proto == 17 && !c ? 0xFFFF : c);
    return tot;
}

/* DL: ch 10 (plain) or the zero-copy ring ch 11 (the CP takes the cell offset the AP put in the slot at wp) */
static void f_dl(struct fakecp *fc, const uint8_t *pk, uint32_t len, int zc)
{
    if (!zc) {
        if (sbm_sbd_put(&fc->sbd, SBM_CH_PDP0, SBM_DL, pk, len, 0) == 0)
            fc->net_dl++;
        return;
    }
    int id = sbm_sbd_id(&fc->sbd, SBM_CH_PDP_HI);
    struct sbm_ring *r = &fc->sbd.r[id][SBM_DL];
    unsigned w = sbm_sbd_wp(&fc->sbd, (unsigned)id, SBM_DL), rp = sbm_sbd_rp(&fc->sbd, (unsigned)id, SBM_DL);
    if (w == rp) {                       /* no empty cell handed over */
        fc->net_zc_full++;
        return;
    }
    uint8_t *slot = fc->sbd.ipc + r->buff_off + w * r->size;
    uint64_t off = (uint64_t)sbm_le32(slot) | (uint64_t)sbm_le32(slot + 4) << 32;
    memcpy(fc->sbd.ipc + off, pk, len);
    sbm_put32(fc->sbd.ipc + r->size_v_off + 4 * w, (len & 0xFFFF) | (uint32_t)SBM_CH_PDP0 << 16);
    uint16_t nw = (uint16_t)((w + 1) % r->len);
    memcpy(fc->sbd.ipc + fc->sbd.rbps_off + 2u * (fc->sbd.n * 3u + (unsigned)id), &nw, 2);   /* DL wp */
    fc->net_dl_zc++;
}

static void f_tcp(struct fakecp *fc, const uint8_t *src_ip, const uint8_t *t, uint32_t tl)
{
    static const char resp[] = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    uint8_t pk[1600];
    uint8_t *o = pk + 20;
    uint8_t fl = t[13];
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4, seq = f_get32(t + 4), dl = tl - hl;
    memset(o, 0, 20);
    f_put16(o, f_get16(t + 2));
    f_put16(o + 2, f_get16(t));
    o[12] = 5 << 4;
    f_put16(o + 14, 65535);
    if (fl & 0x02) {
        fc->net_syn++;
        if (fc->net_drop_first_syn && fc->net_syn == 1)
            return;
        fc->tcp_isn = 0x10000000;
        fc->tcp_rcv = seq + 1;
        fc->tcp_snd = fc->tcp_isn + 1;
        f_put32(o + 4, fc->tcp_isn);
        f_put32(o + 8, fc->tcp_rcv);
        o[13] = 0x12;
        f_dl(fc, pk, f_ip(pk, src_ip, 6, 20), 1);
        fc->tcp_state = 1;
        return;
    }
    if (dl && seq == fc->tcp_rcv && fc->tcp_state == 1) {
        fc->tcp_rcv += dl;
        if (dl > 4 && !memcmp(t + hl, "GET ", 4))
            fc->net_http++;
        /* the response in one segment, then FIN */
        f_put32(o + 4, fc->tcp_snd);
        f_put32(o + 8, fc->tcp_rcv);
        o[13] = 0x18;
        memcpy(o + 20, resp, sizeof(resp) - 1);
        f_dl(fc, pk, f_ip(pk, src_ip, 6, 20 + sizeof(resp) - 1), 1);
        fc->tcp_snd += sizeof(resp) - 1;
        memset(o + 4, 0, 10);
        f_put32(o + 4, fc->tcp_snd);
        f_put32(o + 8, fc->tcp_rcv);
        o[12] = 5 << 4;
        o[13] = 0x11;
        f_dl(fc, pk, f_ip(pk, src_ip, 6, 20), 1);
        fc->tcp_snd += 1;
        fc->tcp_state = 2;
        return;
    }
    if ((fl & 0x01) && fc->tcp_state == 2) {             /* the client's FIN: ACK it */
        fc->tcp_rcv = seq + dl + 1;
        f_put32(o + 4, fc->tcp_snd);
        f_put32(o + 8, fc->tcp_rcv);
        o[13] = 0x10;
        f_dl(fc, pk, f_ip(pk, src_ip, 6, 20), 1);
        fc->tcp_state = 3;
    }
}

static void f_udp_dns(struct fakecp *fc, const uint8_t *src_ip, const uint8_t *u, uint32_t ul)
{
    uint8_t pk[600];
    uint8_t *o = pk + 20;
    const uint8_t *q = u + 8;
    uint32_t ql = ul - 8;
    if (ql < 12 || ql > 300)
        return;
    fc->net_dns++;
    f_put16(o, 53);
    f_put16(o + 2, f_get16(u));
    memcpy(o + 8, q, ql);
    o[8 + 2] = 0x81;
    o[8 + 3] = 0x80;
    f_put16(o + 8 + 6, 1);
    static const uint8_t ans[16] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34 };
    memcpy(o + 8 + ql, ans, sizeof(ans));
    f_put16(o + 4, (uint16_t)(8 + ql + sizeof(ans)));
    f_dl(fc, pk, f_ip(pk, src_ip, 17, 8 + ql + sizeof(ans)), 0);
}

int fakecp_net_step(struct fakecp *fc)
{
    int id = sbm_sbd_id(&fc->sbd, SBM_CH_PDP0), handled = 0;
    if (id < 0)
        return 0;
    for (;;) {
        const uint8_t *f;
        int n = sbm_sbd_get(&fc->sbd, (unsigned)id, SBM_UL, &f, 0);
        if (n == 0)
            break;
        if (n < 0 || (fc->sbd.last_size_v >> 16) != SBM_CH_PDP0 || !fc->pdp_up || n < 28 || (f[0] >> 4) != 4) {
            fc->net_bad++;
            if (n >= 0)
                sbm_sbd_get_done(&fc->sbd, (unsigned)id, SBM_UL, 0);
            continue;
        }
        fc->net_ul++;
        handled++;
        uint32_t ihl = (uint32_t)(f[0] & 15) * 4;
        uint8_t dst[4];
        memcpy(dst, f + 16, 4);
        if (f[9] == 17 && f_get16(f + ihl + 2) == 53)
            f_udp_dns(fc, dst, f + ihl, (uint32_t)n - ihl);
        else if (f[9] == 6)
            f_tcp(fc, dst, f + ihl, (uint32_t)n - ihl);
        else
            fc->net_bad++;
        sbm_sbd_get_done(&fc->sbd, (unsigned)id, SBM_UL, 0);
    }
    return handled;
}
