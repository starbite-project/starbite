/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: Samsung IPC on the FMT channels (docs/MODEM.md section 3.3). One message per SIPC5 frame payload:
 *   u16 length (header included), u8 mseq, u8 aseq, u8 group, u8 index, u8 type, data.
 * A response's aseq is the request's mseq (seen: TX mseq 0x3f -> RX "37 3f 08 05 02"). Group and type values
 * follow the public Samsung IPC naming (libsamsung-ipc, used as a naming reference only) and are confirmed on
 * this phone's captured traffic where sbm.h says "seen".
 *
 * The request builder has an allow-list. Stage 1 may only ask (GET) for status. Nothing in this file can build
 * a CALL, SMS-send, SS, SAT, SIM-access, network-selection or identity request.
 */
#include "sbm.h"

int sbm_ipc_parse(const uint8_t *b, uint32_t len, struct sbm_ipc_msg *m)
{
    sbm_memset(m, 0, sizeof(*m));
    if (!b || len < SBM_IPC_HDR)
        return SBM_ESHORT;
    uint16_t l = sbm_le16(b);
    if (l < SBM_IPC_HDR || l > len)
        return SBM_EBADF;
    m->len = l;
    m->mseq = b[2];
    m->aseq = b[3];
    m->cmd = SBM_CMD(b[4], b[5]);
    m->type = b[6];
    m->data = b + SBM_IPC_HDR;
    m->data_len = (uint16_t)(l - SBM_IPC_HDR);
    return 0;
}

/* What the AP may send. GETs of status only; the data a GET may carry is bounded per command. */
static const struct { uint16_t cmd; uint8_t type; uint8_t max_data; } allowed[] = {
    { SBM_PWR_PHONE_STATE,     SBM_IPC_GET, 0 },
    { SBM_SEC_PIN_STATUS,      SBM_IPC_GET, 0 },
    { SBM_SEC_SIM_ICC_TYPE,    SBM_IPC_GET, 0 },
    { SBM_DISP_ICON_INFO,      SBM_IPC_GET, 1 },   /* icon mask (seen: 0x01) */
    { SBM_DISP_RSSI_INFO,      SBM_IPC_GET, 0 },
    { SBM_NET_PLMN_SEL,        SBM_IPC_GET, 0 },
    { SBM_NET_SERVING_NETWORK, SBM_IPC_GET, 0 },
    { SBM_NET_REGIST,          SBM_IPC_GET, 2 },   /* act, domain (seen: ff 02 / ff 03) */
    { SBM_NET_MODE_SEL,        SBM_IPC_GET, 0 },
    { SBM_MISC_ME_VERSION,     SBM_IPC_GET, 1 },
    { SBM_MISC_TIME_INFO,      SBM_IPC_GET, 0 },
};

static int allowed_idx(uint16_t cmd, uint8_t type)
{
    /* belt and braces: these groups are refused whatever the table says */
    switch (cmd >> 8) {
    case SBM_G_CALL:
    case SBM_G_SMS:
    case SBM_G_SS:
    case SBM_G_SAT:
    case SBM_G_IMEI:
    case SBM_G_RFS:
        return -1;
    default:
        break;
    }
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
        if (allowed[i].cmd == cmd && allowed[i].type == type)
            return (int)i;
    return -1;
}

int sbm_ipc_allowed(uint16_t cmd, uint8_t type) { return allowed_idx(cmd, type) >= 0; }

int sbm_ipc_build(uint8_t *out, uint32_t cap, uint8_t mseq, uint16_t cmd, uint8_t type,
                  const uint8_t *data, uint16_t dlen)
{
    int i = allowed_idx(cmd, type);
    if (i < 0 || dlen > allowed[i].max_data)
        return SBM_EPERM;
    if (dlen && !data)
        return SBM_EINVAL;
    uint32_t l = SBM_IPC_HDR + (uint32_t)dlen;
    if (!out || l > cap)
        return SBM_ENOSPC;
    sbm_put16(out, (uint16_t)l);
    out[2] = mseq;
    out[3] = 0;
    out[4] = (uint8_t)(cmd >> 8);
    out[5] = (uint8_t)cmd;
    out[6] = type;
    if (dlen)
        sbm_memcpy(out + SBM_IPC_HDR, data, dlen);
    return (int)l;
}

int sbm_ipc_frame(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t mseq, uint16_t cmd, uint8_t type,
                  const uint8_t *data, uint16_t dlen)
{
    if (!sbm_ch_is_fmt(ch))
        return SBM_EINVAL;
    if (!out || cap < 4)
        return SBM_ENOSPC;
    int l = sbm_ipc_build(out + 4, cap - 4, mseq, cmd, type, data, dlen);
    if (l < 0)
        return l;
    out[0] = SBM_SIPC5_START;
    out[1] = ch;
    sbm_put16(out + 2, (uint16_t)(l + 4));
    return l + 4;
}

static const struct { uint16_t cmd; const char *name; uint8_t loggable; } names[] = {
    { SBM_PWR_PHONE_PWR_UP,    "PWR_PHONE_PWR_UP", 1 },
    { SBM_CMD(SBM_G_PWR, 0x02), "PWR_PHONE_PWR_OFF", 1 },
    { SBM_CMD(SBM_G_PWR, 0x03), "PWR_PHONE_RESET", 1 },
    { SBM_PWR_PHONE_STATE,     "PWR_PHONE_STATE", 1 },
    { SBM_SEC_PIN_STATUS,      "SEC_PIN_STATUS", 1 },
    { SBM_SEC_PHONE_LOCK,      "SEC_PHONE_LOCK", 1 },
    { SBM_SEC_RSIM_ACCESS,     "SEC_RSIM_ACCESS", 0 },
    { SBM_SEC_SIM_ICC_TYPE,    "SEC_SIM_ICC_TYPE", 1 },
    { SBM_SEC_LOCK_INFO,       "SEC_LOCK_INFO", 1 },
    { SBM_DISP_ICON_INFO,      "DISP_ICON_INFO", 1 },
    { SBM_DISP_RSSI_INFO,      "DISP_RSSI_INFO", 1 },
    { SBM_NET_PREF_PLMN,       "NET_PREF_PLMN", 0 },
    { SBM_NET_PLMN_SEL,        "NET_PLMN_SEL", 1 },
    { SBM_NET_SERVING_NETWORK, "NET_SERVING_NETWORK", 0 },   /* PLMN + LAC */
    { SBM_NET_PLMN_LIST,       "NET_PLMN_LIST", 0 },
    { SBM_NET_REGIST,          "NET_REGIST", 0 },            /* LAC + cell id */
    { SBM_NET_SUBSCRIBER_NUM,  "NET_SUBSCRIBER_NUM", 0 },    /* MSISDN */
    { SBM_NET_MODE_SEL,        "NET_MODE_SEL", 1 },
    { SBM_MISC_ME_VERSION,     "MISC_ME_VERSION", 0 },
    { SBM_MISC_ME_IMSI,        "MISC_ME_IMSI", 0 },
    { SBM_MISC_ME_SN,          "MISC_ME_SN", 0 },            /* IMEI / serial */
    { SBM_MISC_TIME_INFO,      "MISC_TIME_INFO", 1 },
    { SBM_GEN_PHONE_RES,       "GEN_PHONE_RES", 1 },
};

const char *sbm_ipc_name(uint16_t cmd, char buf[12])
{
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (names[i].cmd == cmd)
            return names[i].name;
    sbm_fmt(buf, 12, "0x%04x", cmd);
    return buf;
}

int sbm_ipc_payload_loggable(uint16_t cmd)
{
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (names[i].cmd == cmd)
            return names[i].loggable;
    return 0;   /* unknown: not shown */
}

const char *sbm_ipc_type_name(uint8_t type, int from_cp)
{
    static const char *const ap[] = { "?", "EXEC", "GET", "SET", "CFRM", "EVENT" };
    static const char *const cp[] = { "?", "INDI", "RESP", "NOTI" };
    if (from_cp)
        return type < 4 ? cp[type] : "?";
    return type < 6 ? ap[type] : "?";
}

int sbm_gen_res_decode(const struct sbm_ipc_msg *m, struct sbm_gen_res *r)
{
    sbm_memset(r, 0, sizeof(*r));
    if (m->cmd != SBM_GEN_PHONE_RES || m->data_len < 5)
        return SBM_EBADF;
    r->cmd = SBM_CMD(m->data[0], m->data[1]);
    r->type = m->data[2];
    r->code = sbm_le16(m->data + 3);
    return 0;
}
