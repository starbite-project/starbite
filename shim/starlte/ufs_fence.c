/* SPDX-License-Identifier: GPL-2.0-only
 * StarbiteUFS's fence from the live GPT, written into the XNU payload's ADT placeholders: see ufs_fence.h.
 *
 * The allow-list rule and its evidence:
 *   - USERDATA is the Apple slot and nothing else on UFS is: tools/installer/sbinst/ufslayout.py (layouts
 *     starbite-a and starbite-oneui, S9 #1 since 2026-11-07) "USERDATA 1698432..6613759: bytes 0-16 GiB the iOS
 *     slot, 16 GiB-end the Starbite OS store"; docs/ANDROID-ONEUI.md ("USERDATA (iOS slot + OS store)", One UI's
 *     /data moved to A16DATA, "never USERDATA"); the catalog's iOS 16 root "ufs:USERDATA slot"; commit 30f40f8
 *     (the iOS 27 full root in all of USERDATA). WINESP/WINDOWS are Windows', A16DATA, SYSTEM, VENDOR, ODM One
 *     UI's, SBMAINT the maintenance system's, the rest Samsung's: none is exposed to Apple's software.
 *   - The OS store: store.h / store.c (UFS_STORE_OFFSET) keep it at USERDATA + 16 GiB to the end, and
 *     docs/STORE-ON-CARD.md moves the home store to the card ("card only" or a card store marked HOME), after
 *     which store.c never opens the UFS store again and all of USERDATA belongs to iOS. So: card store home or
 *     card only -> all of USERDATA; otherwise only its first 16 GiB, and the store area is denied by name.
 *   - Every partition must be in gpt_load's table (at most GPT_MAX): a dropped one could not be denied.
 *   - Flags: ROOT (the loader's root candidate), never WRITABLE (M4 and on: read-only; a writable range is a
 *     deliberate later change here, and the driver also wants starbite,writes-allowed and sbufsrw=1).
 * Matched by name, as everything else in the loader (gpt_find), because the installer derives the unique GUIDs
 * per name; the GUID is carried along as the range's UUID.
 *
 * Fails closed: an unreadable or inconsistent GPT (bad header, partitions outside the usable range or
 * overlapping, no or two USERDATA, too many partitions for the deny-list), or UFS's DMA area overlapping what
 * was loaded for XNU, gives an EMPTY allow-list, which the driver reads as "expose nothing".
 */
#include <stdint.h>
#include "ufs_fence.h"
#include "ufs.h"
#include "store.h"

const char *ufs_fence_why;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void zero(uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        p[i] = 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void name16(uint8_t *dst, const char *src)
{
    unsigned i = 0;
    for (; i < 16 && src[i]; ++i)
        dst[i] = (uint8_t)src[i];
    for (; i < 16; ++i)
        dst[i] = 0;
}

static void header(uint8_t *p, uint32_t magic, uint32_t rec, uint32_t count)
{
    put32(p, magic);
    put32(p + 4, 1);
    put32(p + 8, rec);
    put32(p + 12, count);
}

static int deny_add(uint8_t *deny, uint32_t cap, uint32_t *n, uint64_t first, uint64_t count, const char *name)
{
    if (*n >= cap)
        return 0;
    uint8_t *r = deny + 16 + *n * UFS_FENCE_DENY_REC;
    put32(r, 0);
    put32(r + 4, 0);
    put64(r + 8, first);
    put64(r + 16, count);
    name16(r + 24, name);
    ++*n;
    return 1;
}

int ufs_fence_build(const struct ufs_fence_gpt *g, uint8_t *allow, uint32_t alen, uint8_t *deny, uint32_t dlen,
                    const char **why)
{
    const char *w = 0;
    if (alen < 16 || dlen < 16)
        return -1;
    uint32_t acap = (alen - 16) / UFS_FENCE_ALLOW_REC, dcap = (dlen - 16) / UFS_FENCE_DENY_REC;
    if (acap > UFS_FENCE_ALLOW_MAX)
        acap = UFS_FENCE_ALLOW_MAX;
    if (dcap > UFS_FENCE_DENY_MAX)
        dcap = UFS_FENCE_DENY_MAX;
    zero(allow, alen);
    zero(deny, dlen);
    header(allow, UFS_FENCE_ALLOW_MAGIC, UFS_FENCE_ALLOW_REC, 0);
    header(deny, UFS_FENCE_DENY_MAGIC, UFS_FENCE_DENY_REC, 0);

    int ios = -1;
    uint32_t nd = 0;
    if (!g || g->n <= 0) {
        w = "no readable GPT (or more partitions than gpt_load keeps)";
        goto closed;
    }
    if (g->first_usable < 2 || g->last_usable < g->first_usable || g->alt_lba <= g->last_usable) {
        w = "GPT header: usable range";
        goto closed;
    }
    for (int i = 0; i < g->n; ++i) {
        if (!g->name[i][0] || g->first[i] > g->last[i] || g->first[i] < g->first_usable ||
            g->last[i] > g->last_usable) {
            w = "a partition outside the usable range";
            goto closed;
        }
        for (int j = 0; j < i; ++j) {
            if (g->first[i] <= g->last[j] && g->first[j] <= g->last[i]) {
                w = "overlapping partitions";
                goto closed;
            }
        }
        if (same(g->name[i], "USERDATA")) {
            if (ios >= 0) {
                w = "two USERDATA";
                goto closed;
            }
            ios = i;
        }
    }
    if (ios < 0) {
        w = "no USERDATA";
        goto closed;
    }
    if (acap < 1) {
        w = "allow placeholder too small";
        goto closed;
    }
    uint64_t first = g->first[ios], size = g->last[ios] - g->first[ios] + 1, count = size;
    if (!g->ios_whole && size > UFS_FENCE_STORE_OFFSET)
        count = UFS_FENCE_STORE_OFFSET;              /* the UFS store may be live behind the iOS slot */
    if (!deny_add(deny, dcap, &nd, 0, g->first_usable, "GPT-HEAD") ||
        !deny_add(deny, dcap, &nd, g->last_usable + 1, g->alt_lba - g->last_usable, "GPT-TAIL") ||
        (count < size && !deny_add(deny, dcap, &nd, first + count, size - count, "SBSTORE-UFS"))) {
        w = "deny placeholder too small";
        goto closed;
    }
    for (int i = 0; i < g->n; ++i) {
        if (i != ios && !deny_add(deny, dcap, &nd, g->first[i], g->last[i] - g->first[i] + 1, g->name[i])) {
            w = "more partitions than the deny-list holds";
            goto closed;
        }
    }
    uint8_t *r = allow + 16;
    put32(r, 0);                                     /* LU 0 */
    put32(r + 4, 2);                                 /* ROOT; never WRITABLE */
    put64(r + 8, first);
    put64(r + 16, count);
    for (unsigned i = 0; i < 16; ++i)
        r[24 + i] = g->ios_uuid[i];
    name16(r + 40, "USERDATA");
    put64(r + 56, 0);
    put32(allow + 12, 1);
    put32(deny + 12, nd);
    if (why)
        *why = 0;
    return 1;

closed:
    zero(deny + 16, dlen - 16);
    if (why)
        *why = w;
    return 0;
}

/* ---- the ADT (node = u32 nprops, u32 nchildren, props, children; prop = char name[32], u32 length, value
 * padded to 4). Every offset is checked against size: a malformed ADT is left alone. */

#define ADT_MAX_ITEMS 4096U
#define ADT_MAX_DEPTH 32U

static int name_eq(const uint8_t *p, uint32_t cap, const char *s)
{
    uint32_t i = 0;
    for (; s[i]; ++i)
        if (i >= cap || p[i] != (uint8_t)s[i])
            return 0;
    return i < cap && p[i] == 0;
}

/* the offset after the node at off, 0 if malformed */
static uint32_t node_end(const uint8_t *a, uint32_t size, uint32_t off, unsigned depth)
{
    if (depth > ADT_MAX_DEPTH || off > size || size - off < 8)
        return 0;
    uint32_t np = get32(a + off), nc = get32(a + off + 4);
    if (np > ADT_MAX_ITEMS || nc > ADT_MAX_ITEMS)
        return 0;
    off += 8;
    for (uint32_t i = 0; i < np; ++i) {
        if (size - off < 36)
            return 0;
        uint32_t len = get32(a + off + 32) & 0x7fffffffU;
        if (len > size)
            return 0;
        uint32_t adv = 36U + ((len + 3U) & ~3U);
        if (size - off < adv)
            return 0;
        off += adv;
    }
    for (uint32_t i = 0; i < nc; ++i) {
        off = node_end(a, size, off, depth + 1);
        if (!off)
            return 0;
    }
    return off;
}

/* property name of the node at off (already checked by node_end): value offset and length */
static int node_prop(const uint8_t *a, uint32_t off, const char *name, uint32_t *voff, uint32_t *vlen)
{
    uint32_t np = get32(a + off);
    off += 8;
    for (uint32_t i = 0; i < np; ++i) {
        uint32_t raw = get32(a + off + 32), len = raw & 0x7fffffffU;
        if (name_eq(a + off, 32, name)) {
            if (raw & 0x80000000U)
                return 0;                            /* an iBoot-style flag XNU would not mask: not ours */
            *voff = off + 36;
            *vlen = len;
            return 1;
        }
        off += 36U + ((len + 3U) & ~3U);
    }
    return 0;
}

/* the child of the node at off whose "name" is name: its offset, 0 if none */
static uint32_t node_child(const uint8_t *a, uint32_t size, uint32_t off, const char *name)
{
    uint32_t np = get32(a + off), nc = get32(a + off + 4);
    uint32_t c = off + 8;
    for (uint32_t i = 0; i < np; ++i)
        c += 36U + (((get32(a + c + 32) & 0x7fffffffU) + 3U) & ~3U);
    for (uint32_t i = 0; i < nc; ++i) {
        uint32_t end = node_end(a, size, c, 1), voff, vlen;
        if (!end)
            return 0;
        if (node_prop(a, c, "name", &voff, &vlen) && name_eq(a + voff, vlen, name))
            return c;
        c = end;
    }
    return 0;
}

static int placeholder_ok(const uint8_t *v, uint32_t len, uint32_t magic, uint32_t rec, uint32_t max)
{
    return len >= 16 + rec && len <= 16 + rec * max && (len - 16) % rec == 0 && get32(v) == magic &&
           get32(v + 4) == 1 && get32(v + 8) == rec;
}

int ufs_fence_adt_find(const uint8_t *adt, uint32_t size, uint32_t *aoff, uint32_t *alen, uint32_t *doff,
                       uint32_t *dlen)
{
    if (!adt || node_end(adt, size, 0, 0) == 0)
        return 0;
    uint32_t io = node_child(adt, size, 0, "arm-io");
    uint32_t ufs = io ? node_child(adt, size, io, "ufs") : 0;
    if (!ufs || !node_prop(adt, ufs, "starbite,allowed-ranges", aoff, alen) ||
        !node_prop(adt, ufs, "starbite,denied-ranges", doff, dlen))
        return 0;
    return placeholder_ok(adt + *aoff, *alen, UFS_FENCE_ALLOW_MAGIC, UFS_FENCE_ALLOW_REC, UFS_FENCE_ALLOW_MAX) &&
           placeholder_ok(adt + *doff, *dlen, UFS_FENCE_DENY_MAGIC, UFS_FENCE_DENY_REC, UFS_FENCE_DENY_MAX);
}

int ufs_fence_adt_fill(uint8_t *adt, uint32_t size, const struct ufs_fence_gpt *g, const char **why)
{
    uint32_t aoff, alen, doff, dlen;
    if (!ufs_fence_adt_find(adt, size, &aoff, &alen, &doff, &dlen))
        return -1;
    return ufs_fence_build(g, adt + aoff, alen, adt + doff, dlen, why);
}

/* ---- the live GPT: ufs.c's gpt_load (names and ranges) plus the header and USERDATA's GUID ---- */

static uint64_t buf64(uintptr_t p)
{
    uint64_t v = 0;
    for (unsigned i = 8; i; --i)
        v = v << 8 | u8r(p + i - 1);
    return v;
}

static int live_gpt(struct ufs_fence_gpt *g)
{
    uint64_t uf, ul;
    g->n = gpt_load();
    if (g->n <= 0 || !gpt_find("USERDATA", &uf, &ul))
        return 0;
    if (ufs_read(1, 1) || buf64(UFS_BUF) != 0x5452415020494645ULL)          /* "EFI PART" */
        return 0;
    g->alt_lba = buf64(UFS_BUF + 32);
    g->first_usable = buf64(UFS_BUF + 40);
    g->last_usable = buf64(UFS_BUF + 48);
    uint64_t ent = buf64(UFS_BUF + 72);
    uint32_t num = (uint32_t)buf64(UFS_BUF + 80), esz = (uint32_t)(buf64(UFS_BUF + 84) & 0xffffffffU);
    if (esz != 128 || num > 128 || ufs_read(ent, (num * 128U + 4095U) / 4096U))
        return 0;
    int used = 0;
    for (uint32_t i = 0; i < num; ++i) {
        uintptr_t e = UFS_BUF + 128U * i;
        if (!buf64(e) && !buf64(e + 8))
            continue;
        ++used;
        if (buf64(e + 32) != uf || buf64(e + 40) != ul)
            continue;
        /* GPT stores the GUID mixed endian (le32, le16, le16, 8 bytes): to RFC 4122 order */
        static const uint8_t ord[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
        for (unsigned k = 0; k < 16; ++k)
            g->ios_uuid[k] = u8r(e + 16 + ord[k]);
    }
    /* gpt_load keeps at most GPT_MAX entries: a partition it dropped would be missing from the deny-list */
    return used == g->n;
}

int ufs_fence_xnu(uintptr_t adt, uint64_t adt_size, uintptr_t lo, uintptr_t hi)
{
    struct ufs_fence_gpt g;
    uint32_t aoff, alen, doff, dlen;
    ufs_fence_why = 0;
    if (adt_size > (16U << 20) || !ufs_fence_adt_find((const uint8_t *)adt, (uint32_t)adt_size, &aoff, &alen, &doff,
                                                       &dlen))
        return -1;                                   /* no placeholders: exactly as before, UFS untouched */
    zero((uint8_t *)&g, sizeof(g));
    g.n = -1;
    g.name = (const char (*)[24])gpt_name;
    g.first = gpt_first;
    g.last = gpt_last;
    if (lo < UFS_BUF + UFS_BUF_SIZE && UFS_DMA < hi) {
        int r = ufs_fence_build(&g, (uint8_t *)adt + aoff, alen, (uint8_t *)adt + doff, dlen, &ufs_fence_why);
        ufs_fence_why = "UFS DMA area overlaps the loaded kernel";
        return r;
    }
    if (live_gpt(&g)) {
        g.ios_whole = store_card_only() || store_home() == (int)STORE_SD;
    } else {
        g.n = -1;
    }
    return ufs_fence_build(&g, (uint8_t *)adt + aoff, alen, (uint8_t *)adt + doff, dlen, &ufs_fence_why);
}
