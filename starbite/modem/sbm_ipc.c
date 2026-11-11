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

#ifdef SBM_ATTENDED_TX
/* The attended SIM test only (an image built with SBM_ATTENDED_TX, and then only for a session started with
 * SBM_ATTENDED_MAGIC): the requests a registration test needs beyond the status GETs. Each is pinned to its exact
 * data. CALL, SMS, SS, SAT, IMEI and RFS stay refused by allowed_idx's group check, which runs first. */
static const struct { uint16_t cmd; uint8_t type; uint8_t n; uint8_t d[2]; } attended[] = {
    { SBM_PWR_PHONE_STATE, SBM_IPC_EXEC, 2, { 0x02, 0x02 } },   /* radio normal (libsec-ril IpcTxRadioPower 0x0202) */
    { SBM_PWR_PHONE_STATE, SBM_IPC_EXEC, 2, { 0x01, 0x00 } },   /* low power mode: RF off, the CP detaches first */
};

/* the data test's GPRS frames, as libsec-ril builds them for an IPv4 default context without authentication:
 *   DEFINE_PDP SET (149): 01, cid 1, type 02 (IPv4), the APN (1..100 of [A-Za-z0-9.-], zero-padded), all else 0
 *   PDP_CONTEXT SET (112 or 248): act 1 / deactivate 0, cid 1, everything else 0 (no user, password or auth)
 *   PS SET (3): 01 00 00 (attach) */
static int apn_ok(const uint8_t *a)
{
    unsigned k = 0;
    while (k < SBM_APN_MAX && a[k]) {
        char c = (char)a[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
        ++k;
    }
    return k > 0 && k < 101 ? (int)k : 0;
}

static int zeros(const uint8_t *d, unsigned from, unsigned to)
{
    for (unsigned i = from; i < to; ++i)
        if (d[i])
            return 0;
    return 1;
}

static int gprs_ok(uint16_t cmd, uint8_t type, const uint8_t *d, uint16_t n)
{
    if (type != SBM_IPC_SET || !d)
        return 0;
    /* LTE attach profile (libsec-ril IpcTxSetLteAttachProfile): 01, profile 1, type 2/6, APN [3..103], zeros (no
     * address, user, password or auth), the roaming PDP type (legacy [192], long [328]) = the type */
    if (cmd == SBM_GPRS_LTE_ATTACH) {
        unsigned rt = n == SBM_GPRS_ATTACH_LEN ? 192 : 328;
        if ((n != SBM_GPRS_ATTACH_LEN && n != SBM_GPRS_ATTACH_LONG_LEN) || d[0] != 1 || d[1] != 1 ||
            (d[2] != 0x02 && d[2] != 0x06) || d[rt] != d[2])
            return 0;
        unsigned k = (unsigned)apn_ok(d + 3);
        return k && zeros(d, 3 + k, rt) && zeros(d, rt + 1, n);
    }
    if (cmd == SBM_GPRS_PS)
        return n == 3 && d[0] == 1 && d[1] == 0 && d[2] == 0;
    if (cmd == SBM_GPRS_PDP_CONTEXT) {
        if ((n != SBM_GPRS_ACT_LEN && n != SBM_GPRS_ACT_LONG_LEN) || d[0] > 1 || d[1] != 1)
            return 0;
        for (unsigned i = 2; i < n; ++i)
            if (d[i])
                return 0;
        return 1;
    }
    if (n != SBM_GPRS_DEFINE_LEN || d[0] != 1 || d[1] != 1 || d[2] != 0x02)
        return 0;
    unsigned k = 0;
    while (k < SBM_APN_MAX && d[3 + k]) {
        char c = (char)d[3 + k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
        ++k;
    }
    if (k == 0)
        return 0;
    for (unsigned i = 3 + k; i < n; ++i)
        if (d[i])
            return 0;
    return 1;
}

int sbm_ipc_attended_allowed(uint16_t cmd, uint8_t type, const uint8_t *data, uint16_t dlen)
{
    switch (cmd >> 8) {
    case SBM_G_CALL:
    case SBM_G_SMS:
    case SBM_G_SS:
    case SBM_G_SAT:
    case SBM_G_IMEI:
    case SBM_G_RFS:
        return 0;
    default:
        break;
    }
    if (allowed_idx(cmd, type) >= 0)
        return 1;
    if (cmd == SBM_GPRS_DEFINE_PDP || cmd == SBM_GPRS_PDP_CONTEXT || cmd == SBM_GPRS_PS || cmd == SBM_GPRS_LTE_ATTACH)
        return gprs_ok(cmd, type, data, dlen);
    if (cmd == SBM_NET_PLMN_SEL && type == SBM_IPC_SET) {       /* automatic selection: 02, 00 x6, ff */
        static const uint8_t autosel[8] = { 0x02, 0, 0, 0, 0, 0, 0, 0xff };
        return dlen == 8 && data && sbm_memcmp(data, autosel, 8) == 0;
    }
    for (unsigned i = 0; i < sizeof(attended) / sizeof(attended[0]); ++i)
        if (attended[i].cmd == cmd && attended[i].type == type && attended[i].n == dlen && data &&
            sbm_memcmp(attended[i].d, data, dlen) == 0)
            return 1;
    return 0;
}

int sbm_ipc_frame_attended(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t mseq, uint16_t cmd, uint8_t type,
                           const uint8_t *data, uint16_t dlen)
{
    if (allowed_idx(cmd, type) >= 0)
        return sbm_ipc_frame(out, cap, ch, mseq, cmd, type, data, dlen);
    if (!sbm_ipc_attended_allowed(cmd, type, data, dlen))
        return SBM_EPERM;
    if (!sbm_ch_is_fmt(ch))
        return SBM_EINVAL;
    uint32_t l = SBM_IPC_HDR + (uint32_t)dlen;
    if (!out || l + 4 > cap)
        return SBM_ENOSPC;
    sbm_put16(out + 4, (uint16_t)l);
    out[6] = mseq;
    out[7] = 0;
    out[8] = (uint8_t)(cmd >> 8);
    out[9] = (uint8_t)cmd;
    out[10] = type;
    sbm_memcpy(out + 4 + SBM_IPC_HDR, data, dlen);
    out[0] = SBM_SIPC5_START;
    out[1] = ch;
    sbm_put16(out + 2, (uint16_t)(l + 4));
    return (int)l + 4;
}
#endif

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
    { SBM_MISC_TIME_INFO,      "MISC_TIME_INFO", 0 },      /* NITZ: time zone and the PLMN */
    { SBM_GPRS_DEFINE_PDP,     "GPRS_DEFINE_PDP", 0 },     /* the APN */
    { SBM_GPRS_PS,             "GPRS_PS", 1 },
    { SBM_GPRS_PDP_CONTEXT,    "GPRS_PDP_CONTEXT", 0 },
    { SBM_GPRS_IP_CONFIG,      "GPRS_IP_CONFIG", 0 },      /* the phone's address and DNS servers */
    { SBM_GPRS_CALL_STATUS,    "GPRS_CALL_STATUS", 1 },
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
