/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: what the CP says about the SIM and the network, folded into one state (docs/MODEM.md section 3.4), and
 * redaction. Payload shapes are read off this phone's captured FMT traffic (no SIM inserted):
 *   SEC_PIN_STATUS   NOTI  80 00                  status ABSENT, key 0
 *   SEC_SIM_ICC_TYPE NOTI  00
 *   PWR_PHONE_PWR_UP NOTI  53                     (one raw byte)
 *   PWR_PHONE_STATE  NOTI  02
 *   NET_REGIST       NOTI  01 02 04 00 lac(2) cid(4) ...   act GSM, CS, EMERGENCY-only (19 bytes on this
 *                                                 firmware; the first 11 are the classic layout)
 *   NET_SERVING_NETWORK RESP  02 01 ff '0' '0' ...  PLMN as ASCII digits at offset 3
 *   NET_PLMN_SEL     RESP  02                     automatic
 *   DISP_RSSI_INFO   NOTI  3e 19 ff 63 00 ...     (raw; meaning of the bytes unconfirmed)
 *   DISP_ICON_INFO   RESP  01 00 ...              icon mask, bars
 * Location (LAC, cell id) and identities are never kept: only "present".
 */
#include "sbm.h"

void sbm_cell_init(struct sbm_cell_state *st) { sbm_memset(st, 0, sizeof(*st)); }

static int decode_reg(const struct sbm_ipc_msg *m, struct sbm_reg *r)
{
    if (m->data_len < 4)
        return SBM_EBADF;
    sbm_memset(r, 0, sizeof(*r));
    r->act = m->data[0];
    r->domain = m->data[1];
    r->state = m->data[2];
    r->edge = m->data[3];
    r->have_loc = m->data_len >= 10;
    if (m->data_len >= 11)
        r->reject = m->data[10];
    return 0;
}

int sbm_cell_update(struct sbm_cell_state *st, const struct sbm_ipc_msg *m)
{
    st->n_msgs++;
    const uint8_t *d = m->data;
    unsigned n = m->data_len;
    int changed = 0;
#define SET(field, val) do { if ((field) != (val)) { (field) = (val); changed = 1; } } while (0)
    if (m->type != SBM_IPC_NOTI && m->type != SBM_IPC_RESP && m->type != SBM_IPC_INDI) {
        st->n_bad++;
        return 0;
    }
    switch (m->cmd) {
    case SBM_PWR_PHONE_PWR_UP:
        if (n < 1)
            goto bad;
        SET(st->have_pwr, 1);
        SET(st->pwr_up, d[0]);
        break;
    case SBM_PWR_PHONE_STATE:
        if (n < 1)
            goto bad;
        SET(st->have_pwr, 1);
        SET(st->phone_state, d[0]);
        break;
    case SBM_SEC_PIN_STATUS:
        if (n < 2)
            goto bad;
        SET(st->have_sim, 1);
        SET(st->sim_status, d[0]);
        SET(st->sim_key, d[1]);
        break;
    case SBM_SEC_SIM_ICC_TYPE:
        if (n < 1)
            goto bad;
        SET(st->have_icc, 1);
        SET(st->icc_type, d[0]);
        break;
    case SBM_DISP_RSSI_INFO:
        if (n < 1)
            goto bad;
        SET(st->have_rssi, 1);
        SET(st->rssi_raw[0], d[0]);
        SET(st->rssi_raw[1], n > 1 ? d[1] : 0);
        break;
    case SBM_DISP_ICON_INFO:
        if (n < 2)
            goto bad;
        if (d[0] & 0x01) {
            uint8_t bars = d[1] > 5 ? 5 : d[1];
            SET(st->have_bars, 1);
            SET(st->bars, bars);
        }
        break;
    case SBM_NET_REGIST: {
        struct sbm_reg r;
        if (decode_reg(m, &r) < 0)
            goto bad;
        struct sbm_reg *dst = r.domain == SBM_DOMAIN_PS ? &st->ps : &st->cs;
        int *have = r.domain == SBM_DOMAIN_PS ? &st->have_ps : &st->have_cs;
        if (r.domain != SBM_DOMAIN_CS && r.domain != SBM_DOMAIN_PS)
            goto bad;
        if (!*have || dst->act != r.act || dst->state != r.state || dst->reject != r.reject || dst->edge != r.edge ||
            dst->have_loc != r.have_loc)
            changed = 1;
        *have = 1;
        *dst = r;
        break;
    }
    case SBM_NET_SERVING_NETWORK: {
        if (n < 8)
            goto bad;
        char p[7];
        unsigned k = 0;
        for (; k < 6 && 3 + k < n; ++k) {
            char c = (char)d[3 + k];
            if (c < '0' || c > '9')
                break;
            p[k] = c;
        }
        p[k] = 0;
        if (k < 5) {                /* no network (or '#' padding only) */
            SET(st->have_plmn, 0);
            break;
        }
        for (unsigned i = 0; i <= k; ++i)
            if (st->plmn[i] != p[i])
                changed = 1;
        sbm_memcpy(st->plmn, p, k + 1);
        SET(st->have_plmn, 1);
        break;
    }
    case SBM_NET_PLMN_SEL:
        if (n < 1)
            goto bad;
        SET(st->plmn_sel, d[0]);
        break;
    case SBM_GEN_PHONE_RES: {
        struct sbm_gen_res r;
        if (sbm_gen_res_decode(m, &r) < 0)
            goto bad;
        if (!sbm_gen_res_ok(&r))
            st->n_gen_err++;
        break;
    }
    default:
        st->n_unknown++;
        break;
    }
#undef SET
    return changed;
bad:
    st->n_bad++;
    return 0;
}

/* The operator's name for MCC-MNC, for the SCREEN only (never logged): no SIM SPN or NITZ name is read yet. A small
 * table of the Nordic and Baltic networks plus the large ones nearby; 0 when not listed (show the code on screen). */
const char *sbm_operator_name(const char *plmn)
{
    static const struct { char code[7]; const char *name; } t[] = {
        { "24403", "DNA" },      { "24412", "DNA" },     { "24405", "Elisa" },   { "24421", "Elisa" },
        { "24491", "Telia" },    { "24436", "Telia / DNA" }, { "24414", "Alcom" },
        { "24001", "Telia" },    { "24002", "3" },       { "24007", "Tele2" },   { "24008", "Telenor" },
        { "24201", "Telenor" },  { "24202", "Telia" },   { "24214", "ice" },     { "23801", "TDC" },
        { "23802", "Telenor" },  { "23806", "3" },       { "23820", "Telia" },   { "24801", "Telia" },
        { "24802", "Elisa" },    { "24803", "Tele2" },   { "24701", "LMT" },     { "24702", "Tele2" },
        { "24705", "Bite" },     { "24601", "Telia" },   { "24602", "Bite" },    { "24603", "Tele2" },
        { "26201", "Telekom" },  { "26202", "Vodafone" }, { "26203", "O2" },     { "23410", "O2" },
        { "23415", "Vodafone" }, { "23420", "Three" },   { "23430", "EE" },      { "20404", "Vodafone" },
        { "20408", "KPN" },      { "20416", "Odido" },   { "25001", "MTS" },     { "25002", "MegaFon" },
        { "25099", "Beeline" },  { "00101", "Test network" },
    };
    char c[7];
    unsigned n = 0;
    if (!plmn)
        return 0;
    for (unsigned i = 0; plmn[i] && n < 6; ++i)       /* digits only: "24405#" / "24405F" padding dropped */
        if (plmn[i] >= '0' && plmn[i] <= '9')
            c[n++] = plmn[i];
    c[n] = 0;
    if (n < 5)
        return 0;
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
        unsigned k = 0;
        while (t[i].code[k] && t[i].code[k] == c[k])
            ++k;
        if (!t[i].code[k] && !c[k])
            return t[i].name;
    }
    return 0;
}

const char *sbm_sim_name(uint8_t s)
{
    switch (s) {
    case SBM_SIM_READY: return "ready";
    case SBM_SIM_LOCK_SC: return "locked (PIN/PUK)";
    case SBM_SIM_LOCK_FD: return "locked (FDN)";
    case SBM_SIM_LOCK_PN: return "network-locked";
    case SBM_SIM_LOCK_PU: return "locked (PU)";
    case SBM_SIM_LOCK_PP: return "locked (PP)";
    case SBM_SIM_LOCK_PC: return "locked (PC)";
    case SBM_SIM_ABSENT: return "absent";
    case SBM_SIM_ERROR: return "error";
    case SBM_SIM_INIT_COMPLETE: return "ready (init complete)";
    case SBM_SIM_PB_INIT_COMPLETE: return "ready (phonebook)";
    default: return "?";
    }
}

const char *sbm_reg_name(uint8_t s)
{
    switch (s) {
    case SBM_REG_NONE: return "not registered";
    case SBM_REG_HOME: return "home";
    case SBM_REG_SEARCHING: return "searching";
    case SBM_REG_EMERGENCY: return "emergency-only";
    case SBM_REG_UNKNOWN: return "unknown";
    case SBM_REG_ROAMING: return "roaming";
    default: return "?";
    }
}

const char *sbm_act_name(uint8_t a)
{
    switch (a) {
    case SBM_ACT_GSM: return "GSM";
    case SBM_ACT_GPRS: return "GPRS";
    case SBM_ACT_EDGE: return "EDGE";
    case SBM_ACT_UMTS: return "UMTS";
    case SBM_ACT_LTE: return "LTE";
    case 0xFF: return "any";
    default: return "?";
    }
}

void sbm_cell_line(const struct sbm_cell_state *st, char *out, size_t cap)
{
    char sim[32], cs[48], ps[48], plmn[24], bars[16];
    if (st->have_sim)
        sbm_fmt(sim, sizeof(sim), "SIM %s", sbm_sim_name(st->sim_status));
    else
        sbm_strlcpy(sim, "SIM ?", sizeof(sim));
    if (st->have_cs)
        sbm_fmt(cs, sizeof(cs), "CS %s (%s)", sbm_reg_name(st->cs.state), sbm_act_name(st->cs.act));
    else
        sbm_strlcpy(cs, "CS ?", sizeof(cs));
    if (st->have_ps)
        sbm_fmt(ps, sizeof(ps), "PS %s (%s)", sbm_reg_name(st->ps.state), sbm_act_name(st->ps.act));
    else
        sbm_strlcpy(ps, "PS ?", sizeof(ps));
    if (st->have_plmn)
        sbm_fmt(plmn, sizeof(plmn), "PLMN %s", st->plmn);
    else
        sbm_strlcpy(plmn, "PLMN -", sizeof(plmn));
    if (st->have_bars)
        sbm_fmt(bars, sizeof(bars), "bars %u", st->bars);
    else
        sbm_strlcpy(bars, "bars -", sizeof(bars));
    sbm_fmt(out, cap, "%s | %s | %s | %s | %s", sim, cs, ps, plmn, bars);
}

void sbm_cell_line_masked(const struct sbm_cell_state *st, char *out, size_t cap)
{
    struct sbm_cell_state m = *st;
    char line[160], rssi[16];
    m.have_plmn = 0;
    sbm_cell_line(&m, line, sizeof(line));
    if (st->have_rssi)
        sbm_fmt(rssi, sizeof(rssi), " | rssi 0x%02x", st->rssi_raw[0]);
    else
        rssi[0] = 0;
    /* "PLMN -" from the copy: say whether one is known without saying which */
    char *p = line;
    for (; *p; ++p)
        if (p[0] == 'P' && p[1] == 'L' && p[2] == 'M' && p[3] == 'N' && p[4] == ' ' && p[5] == '-')
            break;
    if (*p && st->have_plmn) {
        size_t o = (size_t)(p - line);
        line[o] = 0;
        sbm_fmt(out, cap, "%sPLMN set%s%s", line, p + 6, rssi);
        return;
    }
    sbm_fmt(out, cap, "%s%s", line, rssi);
}

/* ---- redaction ------------------------------------------------------------------------------- */

static int is_dec(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

unsigned sbm_redact_text(char *s, size_t cap)
{
    unsigned masked = 0;
    size_t i = 0;
    while (i < cap && s[i]) {
        /* "0x" literals of up to 16 hex digits are addresses / register values: kept */
        if (s[i] == '0' && i + 1 < cap && (s[i + 1] == 'x' || s[i + 1] == 'X') && (i == 0 || !is_hex(s[i - 1]))) {
            size_t j = i + 2;
            while (j < cap && is_hex(s[j]))
                ++j;
            if (j - i - 2 <= 16) {
                i = j;
                continue;
            }
        }
        if (!is_hex(s[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        unsigned dec = 0;
        while (j < cap && is_hex(s[j])) {
            dec += is_dec(s[j]);
            ++j;
        }
        size_t run = j - i;
        /* a decimal run of >= 7 digits, or a hex run of >= 14 that is mostly digits (BCD identities) */
        int all_dec = dec == run;
        if ((all_dec && run >= 7) || (run >= 14 && dec >= 10)) {
            for (size_t k = i; k < j; ++k)
                s[k] = '#';
            masked++;
        }
        i = j;
    }
    return masked;
}

int sbm_imsi_mccmnc(const uint8_t ef[9], int mnc_len, char out[8])
{
    out[0] = 0;
    if (!ef || ef[0] < 2 || ef[0] > 8)
        return SBM_EBADF;
    if (mnc_len != 3)
        mnc_len = 2;
    /* digit 1 is the high nibble of byte 1 (the low nibble is the parity / type), then low, high, ... */
    char d[16];
    unsigned nd = 0;
    for (unsigned b = 1; b <= ef[0]; ++b) {
        uint8_t lo = ef[b] & 0xF, hi = ef[b] >> 4;
        if (b > 1) {
            if (lo > 9)
                break;
            d[nd++] = (char)('0' + lo);
        }
        if (hi > 9)
            break;
        d[nd++] = (char)('0' + hi);
    }
    if (nd < (unsigned)(3 + mnc_len))
        return SBM_EBADF;
    for (int k = 0; k < 3 + mnc_len; ++k)
        out[k] = d[k];
    out[3 + mnc_len] = 0;
    sbm_memset(d, 0, sizeof(d));
    return (int)nd;
}

void sbm_frame_line(const uint8_t *fr, uint32_t len, int from_cp, char *out, size_t cap)
{
    struct sbm_sipc5 f;
    size_t o = 0;
#define ADD(...) do { if (o < cap) o += (size_t)sbm_fmt(out + o, cap - o, __VA_ARGS__); } while (0)
    if (!cap)
        return;
    out[0] = 0;
    int e = sbm_sipc5_parse(fr, len, &f);
    if (e < 0) {
        ADD("%s bad frame (%s), %u bytes", from_cp ? "RX" : "TX", sbm_strerror(e), len);
        return;
    }
    ADD("%s ch%u len %u", from_cp ? "RX" : "TX", f.ch, f.frame_len);
    if (f.multi)
        ADD(" frag %u%s", f.id, f.more ? "+" : "");
    struct sbm_ipc_msg m;
    if (sbm_ch_is_fmt(f.ch) && !f.multi && sbm_ipc_parse(f.payload, f.payload_len, &m) == 0) {
        char nb[12];
        ADD(" | %s %s seq %u/%u", sbm_ipc_name(m.cmd, nb), sbm_ipc_type_name(m.type, from_cp), m.mseq, m.aseq);
        if (m.data_len && sbm_ipc_payload_loggable(m.cmd)) {
            ADD(" |");
            for (unsigned i = 0; i < m.data_len && i < 16; ++i)
                ADD(" %02x", m.data[i]);
            if (m.data_len > 16)
                ADD(" ..");
        } else if (m.data_len) {
            ADD(" | <%u bytes>", m.data_len);
        }
    } else {
        ADD(" | <%u bytes>", f.payload_len);
    }
#undef ADD
    if (o >= cap)
        out[cap - 1] = 0;
}
