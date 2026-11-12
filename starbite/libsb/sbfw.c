/* SPDX-License-Identifier: GPL-2.0-only
 * The loader <-> OS interface of the firmware layer (docs/STARBITE-FIRMWARE-LAYER.md section 10), libsb's half:
 *   - the SBFW table the loader leaves at 0x91FFE000 before every jump (10.2): checked and copied into sb_fw by
 *     sb_start, before anything could reuse that RAM;
 *   - the SBFW part of the settings record (bytes 112..255, 10.1): the codec, byte for byte the loader's
 *     (shim/starlte/sbfmt.c, "SBFW record and table": sbfw_rec_decode / sbfw_rec_encode / sbset_crc_fix /
 *     sbfw_table_check), so a record either side writes reads the same on the other.
 * Pure code (no hardware) apart from sb_fw_read's one read of the table page: the host test
 * (starbite/host/sbfw-store-test.c) builds it as it is.
 */
#include "sb.h"
#include "store.h"

struct sb_fw sb_fw;

/* ---- small helpers (shim/starlte/sbfmt.c's) ------------------------------------------------------ */

static void z(void *p, unsigned n)
{
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < n; ++i)
        b[i] = 0;
}

static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | p[i - 1];
    return v;
}

static void put(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* NUL-terminated copy of at most n - 1 characters from a fixed field of n bytes. */
static void sget(char *dst, const uint8_t *src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && src[k]; ++k)
        dst[k] = (char)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

static void sput(uint8_t *dst, const char *src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && src[k]; ++k)
        dst[k] = (uint8_t)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

/* ---- SBFW record (settings bytes 112..255) --------------------------------------------------------- */

#define FW_OFF 112U

void sbfw_rec_decode(const uint8_t *b, struct sbfw_rec *r)
{
    z(r, sizeof(*r));
    if (b[FW_OFF] != 'F' || b[FW_OFF + 1] != 'W' || b[FW_OFF + 2] != 1)
        return;
    r->fw_flags = b[115];
    r->req_action = b[116];
    r->req_state = b[117];
    r->req_seq = (uint16_t)get(b + 118, 2);
    sget(r->req_key, b + 120, 24);
    sget(r->shell_key, b + 144, 24);
    for (unsigned i = 0; i < 64; ++i)
        r->undo[i] = b[168 + i];
}

void sbfw_rec_encode(uint8_t *b, const struct sbfw_rec *r)
{
    z(b + FW_OFF, SBSET_BYTES - FW_OFF);
    b[112] = 'F';
    b[113] = 'W';
    b[114] = 1;
    b[115] = r->fw_flags;
    b[116] = r->req_action;
    b[117] = r->req_state;
    put(b + 118, r->req_seq, 2);
    sput(b + 120, r->req_key, 24);
    sput(b + 144, r->shell_key, 24);
    for (unsigned i = 0; i < 64; ++i)
        b[168 + i] = r->undo[i];
}

void sbset_crc_fix(uint8_t *b)
{
    put(b + 12, sb_crc32(b + 16, SBSET_BYTES - 16), 4);
}

/* ---- SBFW table (0x91FFE000) ------------------------------------------------------------------------ */

int sbfw_table_check(const uint8_t *p)
{
    static const char magic[8] = { 'S', 'B', 'F', 'W', 'T', 'B', 'L', '1' };
    for (unsigned i = 0; i < 8; ++i)
        if (p[i] != (uint8_t)magic[i])
            return 0;
    return get(p + 8, 4) == 1 && sb_crc32(p + 16, SBFW_TABLE_SIZE - 16) == (uint32_t)get(p + 12, 4);
}

int sbfw_table_decode(const uint8_t *p, struct sb_fw *f)
{
    z(f, sizeof(*f));
    if (!sbfw_table_check(p))
        return 0;
    f->debug_level = (uint32_t)get(p + 16, 4);
    f->req_action = (uint32_t)get(p + 20, 4);
    sget(f->key, p + 24, 24);
    f->console_y0 = (uint32_t)get(p + 48, 4);
    f->console_y1 = (uint32_t)get(p + 52, 4);
    f->fb_base = get(p + 56, 8);
    f->stride = (uint32_t)get(p + 64, 4);
    f->fb_fmt = (uint32_t)get(p + 68, 4);
    f->loader_version = (uint32_t)get(p + 72, 4);
    f->boot_reason = (uint32_t)get(p + 76, 4);
    f->store_medium = (uint32_t)get(p + 80, 4);
    f->store_flags = (uint32_t)get(p + 84, 4);
    f->store_base = get(p + 88, 8);
    f->store_capacity = get(p + 96, 8);
    f->crash_kind = (uint32_t)get(p + 104, 4);
    f->crash_sev = (uint32_t)get(p + 108, 4);
    sget(f->crash_key, p + 112, 24);
    f->post_fails = (uint32_t)get(p + 136, 4);
    f->post_warns = (uint32_t)get(p + 140, 4);
    f->settings_status = (uint32_t)get(p + 144, 4);
    f->flags = (uint32_t)get(p + 148, 4);
    for (unsigned j = 0; j < SBFW_LINES; ++j)
        sget(f->lines[j], p + 1024 + SBFW_LINE * j, SBFW_LINE);
    f->valid = 1;
    return 1;
}

/* The page is copied out first (one pass of plain loads into the image's .bss), then checked and decoded from the
 * copy: nothing reads the table's RAM again after this. */
static uint8_t fw_page[SBFW_TABLE_SIZE] __attribute__((aligned(8)));

int sb_fw_read(void)
{
    uintptr_t a = SB_SBFW_TABLE;
    /* an opaque address: without it GCC 13 -O2 shares one induction variable between src and fw_page and keeps
     * "fw_page - 0x91ffe000" as a relocated constant, an addend outside the image (check-image.py refuses it) */
    __asm__("" : "+r"(a));
    const volatile uint64_t *src = (const volatile uint64_t *)a;
    uint64_t *dst = (uint64_t *)fw_page;
    for (unsigned i = 0; i < SBFW_TABLE_SIZE / 8U; ++i)
        dst[i] = src[i];
    int ok = sbfw_table_decode(fw_page, &sb_fw);
    struct ub u;
    ub_init(&u);
    if (!ok) {
        sb_log("sbfw: no loader table (an older loader): the store is found as before");
        return 0;
    }
    ub_s(&u, "sbfw: table v1, loader ");
    ub_dec(&u, sb_fw.loader_version);
    ub_s(&u, ", why ");
    ub_dec(&u, sb_fw.boot_reason);
    ub_s(&u, ", key ");
    ub_s(&u, sb_fw.key[0] ? sb_fw.key : "-");
    ub_s(&u, ", level ");
    ub_dec(&u, sb_fw.debug_level);
    ub_s(&u, ", req ");
    ub_dec(&u, sb_fw.req_action);
    ub_s(&u, ", store ");
    ub_s(&u, sb_fw.store_medium == STORE_SD ? "card" : sb_fw.store_medium == STORE_UFS ? "ufs" : "none");
    ub_s(&u, " at ");
    ub_dec(&u, sb_fw.store_base);
    ub_s(&u, (sb_fw.flags & SBFW_TF_CARD_ONLY) ? ", card only" : "");
    ub_s(&u, (sb_fw.flags & SBFW_TF_SHELL) ? ", shell" : "");
    sb_log(u.b);
    return 1;
}
