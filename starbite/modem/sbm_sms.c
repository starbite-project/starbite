/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: SMS PDUs (3GPP TS 23.040 / 23.038) and the incoming-SMS IPC message on this phone (docs/MODEM.md 8).
 * Incoming (IPC 0x0402, from One UI's libsec-ril IpcProtocol41Sms::IpcRxIncomingMsg): data[0] = 2 GSM (1 CDMA),
 * data[5] = the byte count of what follows, then the SMSC address (length octet, TOA, BCD) and the TP-DELIVER PDU.
 * Decoding never logs: the sender and the text are the user's, and the caller decides what to show.
 * Sending: sbm_sms_submit builds only the TP-SUBMIT PDU, and only after sbm_dial_check (the user's allow-list, never an
 * emergency number). The IPC send frame around it (0x0401) is not in this core yet: its first three data bytes are not
 * confirmed (docs/MODEM.md 7.2), so nothing here can send.
 */
#include "sbm.h"

/* GSM 03.38 default alphabet -> Unicode */
static const uint16_t gsm7[128] = {
    '@', 0xA3, '$', 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, '\n', 0xD8, 0xF8, '\r', 0xC5, 0xE5,
    0x394, '_', 0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
    ' ', '!', '"', '#', 0xA4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    0xA1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
    0xBF, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};
static uint16_t gsm7_ext(uint8_t c)
{
    switch (c) {
    case 0x0A: return '\f';
    case 0x14: return '^';
    case 0x28: return '{';
    case 0x29: return '}';
    case 0x2F: return '\\';
    case 0x3C: return '[';
    case 0x3D: return '~';
    case 0x3E: return ']';
    case 0x40: return '|';
    case 0x65: return 0x20AC;
    default: return ' ';
    }
}

static int put_utf8(char *o, size_t cap, size_t *n, uint32_t cp)
{
    uint8_t b[4];
    unsigned k;
    if (cp < 0x80) {
        b[0] = (uint8_t)cp;
        k = 1;
    } else if (cp < 0x800) {
        b[0] = (uint8_t)(0xC0 | cp >> 6);
        b[1] = (uint8_t)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        b[0] = (uint8_t)(0xE0 | cp >> 12);
        b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | cp >> 18);
        b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (uint8_t)(0x80 | (cp & 0x3F));
        k = 4;
    }
    if (*n + k + 1 > cap)
        return -1;
    for (unsigned i = 0; i < k; ++i)
        o[(*n)++] = (char)b[i];
    o[*n] = 0;
    return 0;
}

static uint8_t septet(const uint8_t *ud, uint32_t i)
{
    uint32_t bit = i * 7, by = bit / 8, sh = bit % 8;
    unsigned v = ud[by] >> sh;
    if (sh > 1)
        v |= (unsigned)ud[by + 1] << (8 - sh);
    return (uint8_t)(v & 0x7F);
}

/* address field: digits count, TOA, value; returns the bytes it takes or -1 */
static int addr(const uint8_t *p, uint32_t n, char *out, size_t cap, int *alpha)
{
    if (n < 2)
        return -1;
    uint32_t digits = p[0], bytes = (digits + 1) / 2;
    uint8_t toa = p[1];
    if (2 + bytes > n || cap < 2)
        return -1;
    size_t o = 0;
    out[0] = 0;
    *alpha = (toa & 0x70) == 0x50;
    if (*alpha) {
        for (uint32_t i = 0; i < digits * 4 / 7; ++i) {
            uint8_t c = septet(p + 2, i);
            if (put_utf8(out, cap, &o, c == 0x1B ? ' ' : gsm7[c]) < 0)
                break;
        }
        return (int)(2 + bytes);
    }
    if ((toa & 0x70) == 0x10 && o + 1 < cap)
        out[o++] = '+';
    for (uint32_t i = 0; i < digits && o + 1 < cap; ++i) {
        uint8_t nib = (uint8_t)((p[2 + i / 2] >> ((i & 1) * 4)) & 0xF);
        out[o++] = nib < 10 ? (char)('0' + nib) : nib == 10 ? '*' : nib == 11 ? '#' : '?';
    }
    out[o] = 0;
    return (int)(2 + bytes);
}

int sbm_sms_deliver_decode(const uint8_t *t, uint32_t n, struct sbm_sms *s)
{
    sbm_memset(s, 0, sizeof(*s));
    uint32_t o = 0;
    if (n < 1 || (t[0] & 3) != 0)
        return SBM_EBADF;                            /* not an SMS-DELIVER */
    int udhi = (t[0] & 0x40) != 0;
    o = 1;
    int a = addr(t + o, n - o, s->from, sizeof(s->from), &s->from_alpha);
    if (a < 0)
        return SBM_EBADF;
    o += (uint32_t)a;
    if (o + 2 + 7 + 1 > n)
        return SBM_EBADF;
    s->pid = t[o++];
    s->dcs = t[o++];
    sbm_memcpy(s->scts, t + o, 7);
    o += 7;
    uint32_t udl = t[o++];
    const uint8_t *ud = t + o;
    uint32_t avail = n - o;
    int alphabet;                                    /* 0 GSM 7-bit, 1 8-bit, 2 UCS-2 */
    if ((s->dcs & 0xC0) == 0x00 || (s->dcs & 0xC0) == 0x40)
        alphabet = (s->dcs >> 2) & 3;
    else if ((s->dcs & 0xF0) == 0xF0)
        alphabet = (s->dcs & 0x04) ? 1 : 0;
    else
        alphabet = (s->dcs >> 2) & 3;
    if (alphabet == 3)
        alphabet = 1;
    s->alphabet = (uint8_t)alphabet;
    uint32_t skip_oct = 0;
    if (udhi) {
        if (avail < 1 || ud[0] + 1u > avail)
            return SBM_EBADF;
        uint32_t udhl = ud[0];
        for (uint32_t i = 1; i + 1 < udhl + 1;) {    /* information elements */
            uint8_t iei = ud[i], iel = ud[i + 1];
            if (i + 2 + iel > udhl + 1)
                break;
            if (iei == 0x00 && iel == 3) {
                s->concat = 1;
                s->ref = ud[i + 2];
                s->total = ud[i + 3];
                s->seq = ud[i + 4];
            } else if (iei == 0x08 && iel == 4) {
                s->concat = 1;
                s->ref = ud[i + 3];
                s->total = ud[i + 4];
                s->seq = ud[i + 5];
            }
            i += 2u + iel;
        }
        skip_oct = udhl + 1;
    }
    size_t w = 0;
    s->text[0] = 0;
    if (alphabet == 0) {
        uint32_t need = (udl * 7 + 7) / 8;
        if (need > avail)
            return SBM_EBADF;
        uint32_t first = (skip_oct * 8 + 6) / 7;    /* septets the header takes, with its fill bits */
        for (uint32_t i = first; i < udl; ++i) {
            uint8_t c = septet(ud, i);
            uint32_t cp = gsm7[c];
            if (c == 0x1B && i + 1 < udl)
                cp = gsm7_ext(septet(ud, ++i));
            if (put_utf8(s->text, sizeof(s->text), &w, cp) < 0)
                return SBM_ENOSPC;
        }
    } else {
        if (udl > avail || skip_oct > udl)
            return SBM_EBADF;
        if (alphabet == 2) {
            for (uint32_t i = skip_oct; i + 1 < udl; i += 2) {
                uint32_t cp = (uint32_t)ud[i] << 8 | ud[i + 1];
                if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < udl) {
                    uint32_t lo = (uint32_t)ud[i + 2] << 8 | ud[i + 3];
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
                if (put_utf8(s->text, sizeof(s->text), &w, cp) < 0)
                    return SBM_ENOSPC;
            }
        } else {
            s->binary_len = udl - skip_oct;           /* 8-bit data: not text; its length only */
        }
    }
    return 0;
}

int sbm_sms_incoming(const struct sbm_ipc_msg *m, struct sbm_sms *s)
{
    if (m->cmd != SBM_CMD(SBM_G_SMS, 0x02) || m->data_len < 7 || m->data[0] != 2)
        return SBM_EBADF;
    uint32_t len = m->data[5];
    if (6u + len > m->data_len || len < 1)
        return SBM_EBADF;
    const uint8_t *p = m->data + 6;
    uint32_t sca = p[0];                             /* SMSC: its length octet, then TOA + digits */
    if (1 + sca > len)
        return SBM_EBADF;
    return sbm_sms_deliver_decode(p + 1 + sca, len - 1 - sca, s);
}

/* ASCII -> GSM 7-bit (the common subset; no escape characters): -1 if c has no single septet */
static int to_gsm7(char c)
{
    if (c == '@')
        return 0x00;
    if (c == '$')
        return 0x02;
    if (c == '_')
        return 0x11;
    if (c == '\n')
        return 0x0A;
    if (c == '\r')
        return 0x0D;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return c;
    const char *same = " !\"#%&'()*+,-./:;<=>?";
    for (const char *q = same; *q; ++q)
        if (*q == c)
            return c;
    return -1;
}

int sbm_sms_submit(const struct sbm_dial_policy *pol, const char *number, const char *text, uint8_t *out, uint32_t cap)
{
    int r = sbm_dial_check(pol, number);
    if (r < 0)
        return r;
    uint8_t sept[160];
    uint32_t ns = 0;
    for (const char *c = text; c && *c; ++c) {
        int g = to_gsm7(*c);
        if (g < 0 || ns >= sizeof(sept))
            return SBM_EINVAL;
        sept[ns++] = (uint8_t)g;
    }
    char d[24];
    uint32_t nd = 0;
    int plus = 0;
    for (const char *c = number; *c; ++c) {
        if (*c == '+' && nd == 0)
            plus = 1;
        else if (*c >= '0' && *c <= '9' && nd < sizeof(d))
            d[nd++] = *c;
    }
    uint32_t ud_bytes = (ns * 7 + 7) / 8, need = 2 + 2 + (nd + 1) / 2 + 3 + ud_bytes;
    if (!out || need > cap)
        return SBM_ENOSPC;
    uint32_t o = 0;
    out[o++] = 0x01;                                 /* SMS-SUBMIT, no validity period, no status report */
    out[o++] = 0x00;                                 /* TP-MR: the CP sets it */
    out[o++] = (uint8_t)nd;
    out[o++] = plus ? 0x91 : 0x81;
    for (uint32_t i = 0; i < nd; i += 2)
        out[o++] = (uint8_t)((d[i] - '0') | ((i + 1 < nd ? d[i + 1] - '0' : 0xF) << 4));
    out[o++] = 0x00;                                 /* TP-PID */
    out[o++] = 0x00;                                 /* TP-DCS: GSM 7-bit */
    out[o++] = (uint8_t)ns;                          /* TP-UDL in septets */
    sbm_memset(out + o, 0, ud_bytes);
    for (uint32_t i = 0; i < ns; ++i) {
        uint32_t bit = i * 7, by = bit / 8, sh = bit % 8;
        out[o + by] |= (uint8_t)(sept[i] << sh);
        if (sh > 1)
            out[o + by + 1] |= (uint8_t)(sept[i] >> (8 - sh));
    }
    return (int)(o + ud_bytes);
}

/* ---- SMS-SUBMIT in parts (Starbite OS Next's Messages; docs/MODEM.md 7.11) ----------------------------------------
 * The text (UTF-8) goes as GSM 7-bit when every character is in the default alphabet or its extension table (escape
 * 0x1B), else as UCS-2 (UTF-16, surrogate pairs kept together). One part: 160 septets / 70 units; more: 153 / 67 per
 * part with a concatenation header (IEI 00, an 8-bit reference). The policy (sbm_dial_check) runs first. */
static uint32_t utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0], n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (c >= 0x80 && c < 0xC0)
        return 0xFFFFFFFFu;
    c = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
    for (uint32_t i = 1; i <= n; ++i) {
        if ((s[i] & 0xC0) != 0x80)
            return 0xFFFFFFFFu;
        c = c << 6 | (s[i] & 0x3F);
    }
    *p += n + 1;
    return c;
}

/* septets for one code point into s (1 or 2, the second after the escape); 0 if not in the GSM alphabet */
static unsigned cp_gsm7(uint32_t cp, uint8_t *s)
{
    if (cp == 0x1B)
        return 0;
    for (unsigned i = 0; i < 128; ++i)
        if (gsm7[i] == cp && i != 0x1B) {
            s[0] = (uint8_t)i;
            return 1;
        }
    static const uint8_t ext[] = { 0x0A, 0x14, 0x28, 0x29, 0x2F, 0x3C, 0x3D, 0x3E, 0x40, 0x65 };
    for (unsigned i = 0; i < sizeof(ext); ++i)
        if (gsm7_ext(ext[i]) == cp) {
            s[0] = 0x1B;
            s[1] = ext[i];
            return 2;
        }
    return 0;
}

static uint32_t put_da(uint8_t *out, const char *number)
{
    char d[24];
    uint32_t nd = 0, o = 0;
    int plus = 0;
    for (const char *c = number; *c; ++c) {
        if (*c == '+' && nd == 0)
            plus = 1;
        else if (*c >= '0' && *c <= '9' && nd < sizeof(d))
            d[nd++] = *c;
    }
    out[o++] = (uint8_t)nd;
    out[o++] = plus ? 0x91 : 0x81;
    for (uint32_t i = 0; i < nd; i += 2)
        out[o++] = (uint8_t)((d[i] - '0') | ((i + 1 < nd ? d[i + 1] - '0' : 0xF) << 4));
    return o;
}

int sbm_sms_submit_parts(const struct sbm_dial_policy *pol, const char *number, const char *utf8, uint8_t ref,
                         uint8_t pdu[][SBM_SMS_PDU_MAX], uint16_t *len, unsigned maxparts)
{
    static uint8_t sept[SBM_SMS_MAX_PARTS * 153 + 2];
    static uint16_t ucs[SBM_SMS_MAX_PARTS * 67 + 2];
    int r = sbm_dial_check(pol, number);
    if (r < 0)
        return r;
    if (!utf8 || !*utf8 || !pdu || !len || maxparts == 0)
        return SBM_EINVAL;
    if (maxparts > SBM_SMS_MAX_PARTS)
        maxparts = SBM_SMS_MAX_PARTS;
    /* 7-bit if every character has septets */
    uint32_t ns = 0, nu = 0;
    int gsm = 1, ucs_over = 0;
    for (const char *p = utf8; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0xFFFFFFFFu)
            return SBM_EINVAL;
        uint8_t s2[2];
        unsigned k = gsm ? cp_gsm7(cp, s2) : 0;
        if (!k)
            gsm = 0;
        else if (ns + k <= sizeof(sept)) {
            sept[ns++] = s2[0];
            if (k == 2)
                sept[ns++] = s2[1];
        } else
            return SBM_ERANGE;
        if (nu + (cp > 0xFFFF ? 2 : 1) > sizeof(ucs) / sizeof(ucs[0])) {
            ucs_over = 1;                            /* only an error if the text needs UCS-2 */
        } else if (cp > 0xFFFF) {
            cp -= 0x10000;
            ucs[nu++] = (uint16_t)(0xD800 | (cp >> 10));
            ucs[nu++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            ucs[nu++] = (uint16_t)cp;
        }
    }
    if (!gsm && ucs_over)
        return SBM_ERANGE;
    uint32_t total = gsm ? ns : nu, one = gsm ? 160 : 70, per = gsm ? 153 : 67;
    unsigned parts = total <= one ? 1 : (unsigned)((total + per - 1) / per);
    if (parts > maxparts)
        return SBM_ERANGE;
    uint32_t at = 0;
    for (unsigned p = 0; p < parts; ++p) {
        uint32_t n = parts == 1 ? total : (total - at < per ? total - at : per);
        if (parts > 1 && gsm && n == per && at + n < total && sept[at + n - 1] == 0x1B)
            --n;                                     /* never split an escape from its character */
        if (parts > 1 && !gsm && n == per && at + n < total && (ucs[at + n - 1] & 0xFC00) == 0xD800)
            --n;                                     /* ... nor a surrogate pair */
        uint8_t *o = pdu[p];
        uint32_t q = 0;
        o[q++] = (uint8_t)(0x01 | (parts > 1 ? 0x40 : 0));   /* SMS-SUBMIT (+ UDHI) */
        o[q++] = 0x00;                                       /* TP-MR: the CP sets it */
        q += put_da(o + q, number);
        o[q++] = 0x00;                                       /* TP-PID */
        o[q++] = gsm ? 0x00 : 0x08;                          /* TP-DCS */
        uint32_t udl_at = q++, hdr = parts > 1 ? 6 : 0;
        if (hdr) {
            o[q + 0] = 0x05, o[q + 1] = 0x00, o[q + 2] = 0x03;
            o[q + 3] = ref, o[q + 4] = (uint8_t)parts, o[q + 5] = (uint8_t)(p + 1);
        }
        if (gsm) {
            uint32_t skip = hdr ? 7 : 0;                     /* the header + its fill bit = 7 septets */
            uint32_t sep = skip + n, bytes = (sep * 7 + 7) / 8;
            if (q + bytes > SBM_SMS_PDU_MAX)
                return SBM_ENOSPC;
            sbm_memset(o + q + hdr, 0, bytes - hdr);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t bit = (skip + i) * 7, by = bit / 8, sh = bit % 8;
                o[q + by] |= (uint8_t)(sept[at + i] << sh);
                if (sh > 1)
                    o[q + by + 1] |= (uint8_t)(sept[at + i] >> (8 - sh));
            }
            o[udl_at] = (uint8_t)sep;
            q += bytes;
        } else {
            if (q + hdr + 2 * n > SBM_SMS_PDU_MAX)
                return SBM_ENOSPC;
            q += hdr;
            for (uint32_t i = 0; i < n; ++i) {
                o[q++] = (uint8_t)(ucs[at + i] >> 8);
                o[q++] = (uint8_t)ucs[at + i];
            }
            o[udl_at] = (uint8_t)(hdr + 2 * n);
        }
        len[p] = (uint16_t)q;
        at += n;
    }
    if (at != total)
        return SBM_ERANGE;                           /* the escape / surrogate cuts pushed it past maxparts */
    return (int)parts;
}
