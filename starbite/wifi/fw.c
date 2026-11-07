/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: the files and the firmware's message formats, as pure functions (host-tested against the
 * phone's own files: starbite/wifi/host/).
 *
 *  - NVRAM text to the chip's form: brcmfmac's parser (firmware.c, brcmf_fw_nvram_strip: ISC,
 *    Copyright (c) 2013 Broadcom Corporation), its states reproduced: key=value entries, '#'
 *    comments, RAW1 dropped, "boardrev=0xff" when there is none, NULs, padding to 4 and the length
 *    token ~(len/4) << 16 | len/4. One difference: a last entry without a newline is kept.
 *  - The module's ID from the OTP CIS and the NVRAM it means: facts from Samsung's bcmdhd
 *    (dhd_custom_cis.c dhd_parse_board_information_bcm, vid_info[], bcm4361_naming_table[];
 *    dhd_pcie.c concate_revision_bcm4361), as an earlier port's bcm4361_cis.h lists them.
 *  - clmload chunks, iovars, events: brcmfmac common.c / fwil.c / fweh.h formats.
 */
#include "bcmw_priv.h"

/* ---- NVRAM ------------------------------------------------------------------------------------- */

enum { NV_IDLE, NV_KEY, NV_VALUE, NV_COMMENT, NV_END };

static int nv_char(uint8_t c) { return c != '#' && c >= 0x20 && c < 0x7f; }
static int nv_space(uint8_t c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

int fw_nvram(const uint8_t *text, uint32_t len, uint8_t *out, uint32_t cap, const uint8_t *macaddr)
{
    uint32_t pos = 0, entry = 0, n = 0, total;
    int st = NV_IDLE, boardrev = 0;

    if (len > BCMW_NVRAM_MAX)
        len = BCMW_NVRAM_MAX;
    if (cap < 64)
        return -1;
    while (pos <= len && st != NV_END) {
        uint8_t c = pos < len ? text[pos] : 0;     /* a NUL at the end closes a last value */
        if (pos == len && st != NV_VALUE)
            break;
        switch (st) {
        case NV_IDLE:
            if (c == '\n' || c == '#') {
                st = NV_COMMENT;
            } else if (nv_space(c) || c == 0) {
                pos++;
            } else if (nv_char(c)) {
                entry = pos;
                st = NV_KEY;
            } else {
                pos++;
            }
            break;
        case NV_KEY:
            if (c == '=') {
                const char *k = (const char *)text + entry;
                st = NV_VALUE;
                if (bcmw_strneq(k, "RAW1", 4))
                    st = NV_COMMENT;
                if (bcmw_strneq(k, "boardrev", 8))
                    boardrev = 1;
                if (macaddr && bcmw_strneq(k, "macaddr", 7))
                    st = NV_COMMENT;
                pos++;
            } else if (!nv_char(c) || c == ' ') {
                st = NV_COMMENT;
            } else {
                pos++;
            }
            break;
        case NV_VALUE:
            if (!nv_char(c)) {
                uint32_t l = pos - entry;
                if (n + l + 1 >= cap - 64 || n + l + 1 >= BCMW_NVRAM_MAX) {
                    st = NV_END;
                    break;
                }
                bcmw_memcpy(out + n, text + entry, l);
                n += l;
                out[n++] = 0;
                st = NV_IDLE;
                if (pos == len)
                    pos++;
            } else {
                pos++;
            }
            break;
        case NV_COMMENT:
            while (pos < len && text[pos] != '\n' && text[pos] != 0)
                pos++;
            pos++;
            st = NV_IDLE;
            break;
        }
    }
    if (n == 0)
        return -1;
    if (!boardrev) {
        static const char br[] = "boardrev=0xff";
        bcmw_memcpy(out + n, br, sizeof(br));
        n += sizeof(br);
    }
    if (macaddr) {
        static const char hx[] = "0123456789abcdef";
        static const char k[] = "macaddr=";
        bcmw_memcpy(out + n, k, 8);
        n += 8;
        for (int i = 0; i < 6; i++) {
            out[n++] = (uint8_t)hx[macaddr[i] >> 4];
            out[n++] = (uint8_t)hx[macaddr[i] & 15];
            out[n++] = i < 5 ? ':' : 0;
        }
    }
    total = (n + 1 + 3) & ~3u;
    while (n < total)
        out[n++] = 0;
    put32(out + n, (~(total / 4) << 16) | ((total / 4) & 0xffff));
    return (int)(n + 4);
}

const char *fw_nvram_get(const uint8_t *nv, uint32_t len, const char *key)
{
    uint32_t kl = bcmw_strlen(key), i = 0;
    while (i < len && nv[i]) {
        const char *e = (const char *)nv + i;
        uint32_t el = bcmw_strlen(e);
        if (el > kl && bcmw_strneq(e, key, kl) && e[kl] == '=')
            return e + kl + 1;
        i += el + 1;
    }
    return 0;
}

/* ---- the module from OTP ------------------------------------------------------------------------ */

static const struct { uint8_t rev, maker; const char *suffix; } vids[] = {
    { 0x51, 0x33, "_r01d_b0" }, { 0x61, 0x33, "_r01f_b0" }, { 0x62, 0x33, "_r02g_b0" }, { 0x71, 0x33, "_r01h_b0" },
    { 0x81, 0x33, "_r01i_b0" }, { 0x82, 0x33, "_r02j_b0" },
    { 0x91, 0x33, "_r02a_b2" }, { 0xa1, 0x33, "_r02b_b2" },
    { 0x13, 0x22, "_murata_r013_b0" }, { 0x14, 0x22, "_murata_r014_b0" }, { 0x15, 0x22, "_murata_r015_b0" },
    { 0x20, 0x22, "_murata_r020_b0" }, { 0x21, 0x22, "_murata_r021_b0" }, { 0x22, 0x22, "_murata_r022_b0" },
    { 0x23, 0x22, "_murata_r023_b0" }, { 0x24, 0x22, "_murata_r024_b0" }, { 0x30, 0x22, "_murata_r030_b0" },
    { 0x31, 0x22, "_murata_r030_b0" },     /* bcmdhd: "exceptional case: r31 -> r30" */
    { 0x32, 0x22, "_murata_r032_b0" }, { 0x33, 0x22, "_murata_r033_b0" }, { 0x34, 0x22, "_murata_r034_b0" },
    { 0x50, 0x22, "_murata_r020_b2" }, { 0x51, 0x22, "_murata_r021_b2" }, { 0x52, 0x22, "_murata_r022_b2" },
    { 0x61, 0x22, "_murata_r031_b2" },
};

int fw_cis_parse(const uint8_t *cis, uint32_t length, struct bcm4361_module *m)
{
    uint32_t at = 0;

    bcmw_memset(m, 0, sizeof(*m));
    m->boardtype = -1;
    if (length < 3 || cis[0] != 0x80)
        return 0;
    while (at + 2 <= length) {
        uint32_t l = cis[at + 1];
        if (at + 2 + l > length)
            break;
        if (l >= 1 && cis[at + 2] == 0x81 && m->vid_length == 0) {
            m->vid_length = l - 1 > sizeof(m->vid) ? sizeof(m->vid) : l - 1;
            bcmw_memcpy(m->vid, cis + at + 3, m->vid_length);
        } else if (l >= 2 && cis[at + 2] == 0x1b && m->boardtype < 0) {
            m->boardtype = cis[at + 3];
        }
        if (m->vid_length && m->boardtype >= 0)
            break;
        at += 2 + l;
    }
    return m->vid_length != 0;
}

void fw_module_files(struct bcm4361_module *m, const char *rev_tag)
{
    m->maker[0] = m->nvram[0] = 0;
    m->known = 0;
    if (m->vid_length < 2 || !rev_tag)
        return;
    if (m->vid[1] == 0x33)
        bcmw_strlcpy(m->maker, "semco", sizeof(m->maker));
    else if (m->vid[1] == 0x22)
        bcmw_strlcpy(m->maker, "murata", sizeof(m->maker));
    else
        return;
    for (unsigned i = 0; i < sizeof(vids) / sizeof(vids[0]); i++) {
        uint32_t sl = bcmw_strlen(vids[i].suffix);
        if (m->vid_length == 2 && vids[i].rev == m->vid[0] && vids[i].maker == m->vid[1] &&
            bcmw_strneq(vids[i].suffix + sl - 2, rev_tag, 3)) {
            bcmw_strlcpy(m->nvram, "nvram.txt", sizeof(m->nvram));
            bcmw_strlcpy(m->nvram + 9, vids[i].suffix, sizeof(m->nvram) - 9);
            m->known = 1;
            return;
        }
    }
    /* a VID the table does not have: the maker's only B2 NVRAM among the vendor's files */
    if (bcmw_strneq(rev_tag, "b2", 3))
        bcmw_strlcpy(m->nvram, m->vid[1] == 0x33 ? "nvram.txt_r02b_b2" : "nvram.txt_murata_r041_b2", sizeof(m->nvram));
}

const char *fw_rev_tag(uint32_t chiprev)
{
    if (chiprev >= 5)
        return "b2";
    if (chiprev >= 3)
        return "b0";
    return 0;
}

/* ---- the firmware image -------------------------------------------------------------------------- */

int fw_image_check(const uint8_t *fw, uint32_t len, struct fw_image_info *fi)
{
    static const char key[] = "Version: ";
    bcmw_memset(fi, 0, sizeof(*fi));
    if (len < 0x200 || len > 4u << 20)
        return BCMW_E_BADFILE;
    fi->rstvec = le32(fw);
    if (le32(fw + 0x6c) == 0x534d4152)          /* "SMAR": the RAM size the firmware wants */
        fi->ramsize_override = le32(fw + 0x70);
    /* Broadcom's banner: "<chip>-roml/<config> Version: 13.50.37 (...) CRC: ... FWID: 01-..." */
    for (uint32_t i = len > 0x40000 ? len - 0x40000 : 0; i + sizeof(key) < len; i++) {
        if (fw[i] == 'V' && bcmw_strneq((const char *)fw + i, key, sizeof(key) - 1)) {
            uint32_t s = i, e = i, n = 0;
            while (s > 0 && fw[s - 1] >= 0x20 && fw[s - 1] < 0x7f && i - s < 64)
                s--;
            while (e < len && fw[e] >= 0x20 && fw[e] < 0x7f)
                e++;
            for (uint32_t j = s; j < e && n + 1 < sizeof(fi->version); j++)
                fi->version[n++] = (char)fw[j];
            fi->version[n] = 0;
            break;
        }
    }
    return BCMW_OK;
}

int fw_clm_check(const uint8_t *clm, uint32_t len)
{
    return len > 64 && bcmw_strneq((const char *)clm, "BLOB", 4);
}

/* ---- commands ------------------------------------------------------------------------------------ */

uint32_t fw_iovar(uint8_t *out, uint32_t cap, const char *name, const void *data, uint32_t len)
{
    uint32_t nl = bcmw_strlen(name) + 1;
    if (nl + len > cap)
        return 0;
    bcmw_memcpy(out, name, nl);
    if (data && len)
        bcmw_memcpy(out + nl, data, len);
    else if (len)
        bcmw_memset(out + nl, 0, len);
    return nl + len;
}

#define DL_BEGIN 0x0002
#define DL_END 0x0004
#define DLOAD_HANDLER_VER 1
#define DLOAD_FLAG_VER_SHIFT 12
#define DL_TYPE_CLM 2

uint32_t fw_clm_chunk(uint8_t *out, const uint8_t *data, uint32_t off, uint32_t total, uint32_t max_chunk)
{
    uint32_t n = total - off > max_chunk ? max_chunk : total - off;
    uint16_t flag = (uint16_t)(DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT);
    if (off == 0)
        flag |= DL_BEGIN;
    if (off + n == total)
        flag |= DL_END;
    put16(out, flag);
    put16(out + 2, DL_TYPE_CLM);
    put32(out + 4, n);
    put32(out + 8, 0);                          /* crc: 0, as brcmfmac */
    bcmw_memcpy(out + 12, data + off, n);
    return 12 + n;
}

/* ---- events: Ethernet (14) + Broadcom header (10) + event message (48), big-endian ------------- */

int fw_event_parse(const uint8_t *f, uint32_t len, struct bcmw_event *ev)
{
    static const uint8_t oui[3] = { 0x00, 0x10, 0x18 };
    const uint8_t *m;

    if (len < 14 + 10 + 48 || be16(f + 12) != 0x886c)          /* ETH_P_LINK_CTL */
        return -1;
    if (be16(f + 14) != 0x8001 || f[18] != 0 || f[19] != oui[0] || f[20] != oui[1] || f[21] != oui[2] ||
        be16(f + 22) != 1)
        return -1;
    m = f + 24;
    bcmw_memset(ev, 0, sizeof(*ev));
    ev->flags = be16(m + 2);
    ev->type = be32(m + 4);
    ev->status = be32(m + 8);
    ev->reason = be32(m + 12);
    ev->auth_type = be32(m + 16);
    ev->datalen = be32(m + 20);
    bcmw_memcpy(ev->addr, m + 24, 6);
    bcmw_memcpy(ev->ifname, m + 30, 16);
    ev->ifname[16] = 0;
    ev->ifidx = m[46];
    ev->bsscfgidx = m[47];
    if (ev->datalen > len - 72)
        ev->datalen = len - 72;
    ev->data = f + 72;
    return 0;
}
