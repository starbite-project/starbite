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

/* ---- SHA-256 (FIPS 180-4), to know the files are the ones the phone ships ------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

void fw_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t last[128];
    uint32_t full = len & ~63u, rest = len - full, n;
    for (uint32_t i = 0; i < full; i += 64)
        sha_block(h, data + i);
    bcmw_memset(last, 0, sizeof(last));
    bcmw_memcpy(last, data + full, rest);
    last[rest] = 0x80;
    n = rest + 9 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        last[n - 1 - i] = (uint8_t)((uint64_t)len * 8 >> (8 * i));
    sha_block(h, last);
    if (n == 128)
        sha_block(h, last + 64);
    for (int i = 0; i < 8; i++)
        out[4 * i] = (uint8_t)(h[i] >> 24), out[4 * i + 1] = (uint8_t)(h[i] >> 16), out[4 * i + 2] = (uint8_t)(h[i] >> 8),
        out[4 * i + 3] = (uint8_t)h[i];
}

/* The radio's files of the phone's two known vendor releases: One UI 8 / LineageOS 24 (13.50.37, S9 #1's
 * VENDOR) and the older release in the pmOS/ChromeOS images (13.50.28). Full SHA-256 sums, generated from
 * `sha256sum` output (a hand-typed prefix once refused the right NVRAM on the phone, run 1). */
static const struct { uint8_t sum[32]; const char *what; } known[] = {
    { { 0xe3, 0xc6, 0x64, 0x40, 0xa4, 0xa6, 0xc3, 0xd3, 0x09, 0x18, 0xf9, 0x22, 0xc2, 0x0c, 0xcd, 0x0f, 0xbe, 0xca, 0xa1, 0xaa, 0xa0, 0x46, 0x0b, 0x44, 0x58, 0xb4, 0x8e, 0x20, 0x24, 0x73, 0xf4, 0x52 },
      "bcmdhd_sta.bin_b2 13.50.37" },
    { { 0xc5, 0xbc, 0xfa, 0xf7, 0x6e, 0xdc, 0x5c, 0x15, 0xb3, 0x6a, 0x71, 0x82, 0xb4, 0x03, 0x93, 0x68, 0x4e, 0x94, 0x8b, 0xc3, 0x7e, 0xd1, 0x3a, 0x16, 0xf5, 0x4f, 0x2b, 0xe1, 0x5a, 0xbe, 0x67, 0xab },
      "bcmdhd_sta.bin_b2 13.50.28" },
    { { 0x17, 0x1e, 0x66, 0xd6, 0x1a, 0xc3, 0xe8, 0x38, 0xfd, 0xb3, 0xfa, 0x62, 0xfd, 0x5d, 0xd6, 0xc8, 0xe5, 0x90, 0xdb, 0x19, 0x22, 0x47, 0xda, 0xc1, 0xa8, 0xc6, 0x0a, 0x36, 0x4e, 0x30, 0xac, 0x69 },
      "bcmdhd_sta.bin_b0 13.38.62" },
    { { 0x5b, 0xb5, 0x74, 0xb0, 0xf3, 0xde, 0xbe, 0x08, 0x08, 0x55, 0xf0, 0x0c, 0xde, 0x03, 0x81, 0x41, 0x6a, 0xec, 0xa7, 0xfa, 0xfc, 0x4a, 0x3f, 0x4a, 0x48, 0x8b, 0xbe, 0x2e, 0x6b, 0x5d, 0x8b, 0xe9 },
      "nvram.txt_r02a_b2" },
    { { 0x68, 0xcb, 0x90, 0x39, 0x37, 0xa5, 0xc2, 0xaf, 0x05, 0x5b, 0xee, 0x36, 0x14, 0x10, 0x47, 0xdf, 0x9c, 0xf5, 0xe7, 0xd7, 0x9a, 0x23, 0xa2, 0x4a, 0x06, 0x77, 0xe2, 0xfa, 0xf8, 0x5c, 0xcb, 0xa2 },
      "nvram.txt_r02b_b2" },
    { { 0x6b, 0xf8, 0x29, 0x55, 0x03, 0x77, 0x50, 0xe1, 0xcd, 0x95, 0x34, 0xd5, 0x24, 0xa6, 0x2e, 0xa0, 0x72, 0xb0, 0x70, 0x58, 0xc5, 0xa5, 0x79, 0x70, 0x30, 0x03, 0x0d, 0xdd, 0xe5, 0x9c, 0x01, 0x73 },
      "nvram.txt_murata_r041_b2" },
    { { 0x96, 0x8b, 0x71, 0xeb, 0x19, 0xfc, 0x80, 0x53, 0xe9, 0xa9, 0xbf, 0xb4, 0x4e, 0x59, 0x15, 0x63, 0xf3, 0xf4, 0xa2, 0xb4, 0x62, 0x32, 0x91, 0x2d, 0x76, 0x0a, 0x1c, 0x6b, 0xa0, 0x62, 0x66, 0x92 },
      "nvram.txt_r02a_b2 (older)" },
    { { 0xdb, 0x33, 0xf8, 0x04, 0x37, 0x7e, 0x19, 0x68, 0xdf, 0x01, 0xe7, 0xdf, 0x92, 0xe8, 0x35, 0xd6, 0x4b, 0xa0, 0xa9, 0xbe, 0x78, 0xd3, 0xbe, 0x19, 0x27, 0x9b, 0x51, 0xb7, 0x08, 0xd7, 0x83, 0x0b },
      "nvram.txt_r02b_b2 (older)" },
    { { 0xcf, 0x77, 0x03, 0xbf, 0xe8, 0xaa, 0xe0, 0x09, 0xc2, 0xf1, 0x45, 0xf9, 0x24, 0x90, 0x4b, 0xe0, 0x34, 0x1e, 0xde, 0x77, 0x0a, 0x49, 0xd5, 0x69, 0xd7, 0x5b, 0xdc, 0x02, 0x17, 0x74, 0x56, 0x45 },
      "nvram.txt_murata_r041_b2 (older)" },
    { { 0xd2, 0x97, 0xac, 0x8e, 0xa2, 0xa5, 0x91, 0xc5, 0xd7, 0x43, 0xc0, 0xb7, 0xee, 0x8e, 0xec, 0xf9, 0xf8, 0xb3, 0x17, 0x69, 0x35, 0xa1, 0x80, 0x63, 0x66, 0x10, 0x01, 0xb7, 0x78, 0xf3, 0xa6, 0xfd },
      "nvram.txt_murata_r031_b0 (older)" },
    { { 0x43, 0xde, 0x8a, 0xe9, 0x8a, 0x1f, 0xa3, 0xaf, 0xe1, 0x1f, 0xed, 0x05, 0x36, 0xda, 0xbf, 0xce, 0xe2, 0xc1, 0xce, 0xe5, 0xa7, 0xb1, 0xe0, 0x8a, 0xdc, 0x77, 0xae, 0x53, 0xb7, 0x80, 0x6f, 0x20 },
      "nvram.txt_murata_r032_b0 (older)" },
    { { 0x30, 0x67, 0x4f, 0x2c, 0xec, 0x54, 0x31, 0x3c, 0xf7, 0x61, 0x0f, 0x94, 0x08, 0x47, 0x6f, 0xa8, 0x36, 0xac, 0x77, 0x66, 0x01, 0x79, 0xf2, 0x2e, 0xdf, 0x1c, 0x93, 0xc0, 0x2e, 0xfb, 0x4d, 0x84 },
      "nvram.txt_murata_r033_b0 (older)" },
    { { 0x83, 0x8b, 0xaa, 0x9a, 0x23, 0xf4, 0xfe, 0x5f, 0xb4, 0x80, 0xbd, 0x65, 0xdd, 0x01, 0x67, 0x90, 0x3e, 0xd0, 0xac, 0xdf, 0xb4, 0x0e, 0x38, 0xd3, 0x61, 0x92, 0xea, 0x41, 0x8e, 0xde, 0x71, 0x5e },
      "nvram.txt_r02j_b0 (older)" },
    { { 0x1c, 0x14, 0x77, 0x86, 0x4b, 0x08, 0x58, 0xbd, 0x0a, 0xd8, 0x70, 0x0a, 0x76, 0x63, 0x18, 0xc2, 0xf7, 0x22, 0x0e, 0x47, 0x66, 0xdb, 0xa0, 0xe8, 0xee, 0x15, 0xd8, 0xbf, 0x13, 0x43, 0x53, 0x56 },
      "bcmdhd_clm.blob" },
    { { 0x2d, 0x9d, 0xc4, 0x98, 0x0c, 0x65, 0x44, 0xa2, 0xd4, 0xeb, 0x1a, 0x11, 0x1f, 0x55, 0xd0, 0x2b, 0xaa, 0x26, 0xec, 0xce, 0xe5, 0x79, 0x8a, 0x66, 0xa7, 0x7b, 0x8f, 0xc1, 0x05, 0xca, 0xc2, 0x4b },
      "bcmdhd_clm.blob (older)" },
};

const char *fw_known(const uint8_t sum[32])
{
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        unsigned k = 0;
        while (k < 32 && known[i].sum[k] == sum[k])
            k++;
        if (k == 32)
            return known[i].what;
    }
    return 0;
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
