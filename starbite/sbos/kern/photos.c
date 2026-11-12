/* SPDX-License-Identifier: GPL-2.0-only
 * Track A: the Camera's photo store. Not a filesystem (the card's exFAT stays read only): the OS's own volatile
 * home-store entry "sbos-photos" (64 MiB, made on the Mac with
 * `tools/store/store-update.sh --volatile sbos-photos=67108864`), a ring of 32 self-describing slots of 2 MiB.
 *
 *   slot k = blocks k * 512 .. k * 512 + 511 of the entry
 *   block 0  the header (little endian; the rest of the block zero):
 *              0 "SBPHOTO1"   8 u32 version 1   12 u32 CRC32 of bytes 0..63 with 12..15 zero
 *             16 u64 seq (1, 2, ...: newest = highest; 0 = deleted)   24 u32 w   28 u32 h
 *             32 u32 format (0 RGB565 little endian, 1 24-bit R, G, B: what this file writes)
 *             36 i64 utc (s since 1970, 0 unknown)   44 u32 CRC32 of the pixel bytes   48 u32 pixel bytes (w * h * bpp)
 *   block 1  the pixels, row after row, no padding (at most 2 MiB - 4 KiB = 2093056 bytes)
 *
 * The format is 24-bit RGB: the Camera's still (612 x 816, the same as its BMP) is kept exactly, 1.5 MB. A picture
 * of more than KPHOTO_MAX_PX = 697685 pixels does not fit and is scaled down (a box filter, the aspect kept) to the
 * largest size that does: 1080 x 1440 is kept as 723 x 964 (kphotos_fit). Sides up to KPHOTO_MAX_SIDE (4096).
 *
 * Save: the slot is the first free one (no valid header, deleted, or found damaged), else the oldest (lowest seq).
 * A slot that holds a header is first made invalid (a zero block 0), then the pixels are written, then the header
 * LAST; so a power loss at any point leaves either the old photo or no photo in that slot, never a header over
 * other pixels. The pixels are read back on UFS (forced unit access, no read-back in the fence); on the card the
 * fence's writer has read every block back already. The header is read back on both. Delete: the header written
 * again with seq 0. Count / get: the 32 headers are read once (32 one-block reads) and kept; saves and deletes
 * update that copy. Get reads the photo's pixels and checks their CRC32: a damaged photo (e.g. a torn write) is
 * skipped (marked free; the photos after it move up one) and the count drops by one.
 *
 * Time on the phone (the card, PIO ~10..20 MB/s): a 612 x 816 save writes 368 blocks + 2 = ~1.5 MB and the fence
 * reads them back: about 0.2..0.4 s in one call; on UFS ~30 ms. A get reads 1.5 MB: ~0.1 s on the card. The first
 * count reads 32 blocks: ~30 ms.
 *
 * Writes: the fence has ONE standing grant, the state slot's (slot.c): each write takes a grant for exactly the
 * slot's window (store_volatile_grant_part, 512 blocks) and lends (hw_slot_lend); slot.c takes it back at its next
 * write. No write while storage is held (sb_storage_held: the modem runs; the fence's volatile writer does not check
 * that itself) or in a safe start. In a self-test image (sbos_selftest_enabled) saves and deletes are logged dry
 * runs. The host test kern/host/photos-test.c builds this file with fakes around it. */
#include "sb.h"
#include "kern.h"
#include "store.h"

#define PH_NAME "sbos-photos"
#define PH_ENTRY ((uint64_t)KPHOTO_SLOTS * KPHOTO_SLOT_BYTES)    /* 64 MiB */
#define PH_SLOT_BLOCKS (KPHOTO_SLOT_BYTES / 4096U)              /* 512 */
#define PH_DATA_MAX (KPHOTO_SLOT_BYTES - 4096U)
#define PH_VER 1U
#define PH_HDR 64U                                              /* the bytes under the header's CRC */

struct ph {
    int valid;                                  /* block 0 holds a well-formed header (a photo or a deleted one) */
    int ok;                                     /* a photo: valid, seq > 0, not found damaged */
    uint64_t seq;
    uint32_t w, h, fmt, dcrc, n;
    int64_t utc;
};

static struct {
    int scanned;                                /* the headers below were read (since the last reset) */
    struct store_ent e;
    unsigned med;                               /* SB_MED_* of the entry */
    struct ph s[KPHOTO_SLOTS];
    int order[KPHOTO_SLOTS], n;                 /* the photos' slots, newest first */
    const char *why;
} P;
static uint8_t hb[4096], rb[4096];
static int force_dry;

void kphotos_dry(int on) { force_dry = on; }
static int dry(void) { return force_dry || sbos_selftest_enabled; }
const char *kphotos_why(void) { return P.why ? P.why : ""; }

static int fail(const char *why)
{
    P.why = why;
    sb_logf("photos: ", why);
    return -1;
}

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static uint32_t hdr_crc(const uint8_t *b)
{
    uint8_t t[PH_HDR];
    for (unsigned i = 0; i < PH_HDR; i++) t[i] = b[i];
    put32(t + 12, 0);
    return sb_crc32(t, PH_HDR);
}

static void hdr_build(uint8_t *b, const struct ph *p)
{
    for (unsigned i = 0; i < 4096U; i++) b[i] = 0;
    const char *m = "SBPHOTO1";
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)m[i];
    put32(b + 8, PH_VER);
    put64(b + 16, p->seq);
    put32(b + 24, p->w);
    put32(b + 28, p->h);
    put32(b + 32, p->fmt);
    put64(b + 36, (uint64_t)p->utc);
    put32(b + 44, p->dcrc);
    put32(b + 48, p->n);
    put32(b + 12, hdr_crc(b));
}

/* 1 a well-formed header (p filled; p->ok: a photo), 0 not */
static int hdr_parse(const uint8_t *b, struct ph *p)
{
    const char *m = "SBPHOTO1";
    uint8_t *z = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof(*p); i++) z[i] = 0;
    for (int i = 0; i < 8; i++)
        if (b[i] != (uint8_t)m[i]) return 0;
    if (get32(b + 8) != PH_VER || get32(b + 12) != hdr_crc(b)) return 0;
    p->seq = get64(b + 16);
    p->w = get32(b + 24);
    p->h = get32(b + 28);
    p->fmt = get32(b + 32);
    p->utc = (int64_t)get64(b + 36);
    p->dcrc = get32(b + 44);
    p->n = get32(b + 48);
    uint32_t bpp = p->fmt == 0 ? 2U : p->fmt == 1 ? 3U : 0U;
    if (!bpp || !p->w || !p->h || p->w > KPHOTO_MAX_SIDE || p->h > KPHOTO_MAX_SIDE ||
        (uint64_t)p->w * p->h * bpp != p->n || p->n > PH_DATA_MAX)
        return 0;
    p->valid = 1;
    p->ok = p->seq != 0;
    return 1;
}

static void sort(void)
{
    P.n = 0;
    for (int k = 0; k < KPHOTO_SLOTS; k++) {
        if (!P.s[k].ok) continue;
        int j = P.n++;
        while (j > 0 && P.s[P.order[j - 1]].seq < P.s[k].seq) {
            P.order[j] = P.order[j - 1];
            j--;
        }
        P.order[j] = k;
    }
}

/* the entry (every call: the directory is in RAM once opened); 1 found */
static int entry(void)
{
    struct store_ent e;
    if (!store_find("home:" PH_NAME, &e) || !(e.flags & 1U) || e.size < PH_ENTRY || (e.size & 4095U)) {
        P.scanned = 0;
        P.why = "This phone has no photo storage (the store entry sbos-photos).";
        return 0;
    }
    if (P.scanned && (e.lba != P.e.lba || e.medium != P.e.medium)) P.scanned = 0;
    P.e = e;
    P.med = e.medium == STORE_SD ? SB_MED_SD : SB_MED_UFS;
    return 1;
}

static uint64_t slot_lba(int k) { return P.e.lba + (uint64_t)k * PH_SLOT_BLOCKS; }

static int scan(void)
{
    if (!entry()) return 0;
    if (P.scanned) return 1;
    int bad = 0;
    for (int k = 0; k < KPHOTO_SLOTS; k++)
        if (sb_blk_read(P.med, 0, slot_lba(k), 1, hb) || !hdr_parse(hb, &P.s[k])) {
            uint8_t *z = (uint8_t *)&P.s[k];
            for (unsigned i = 0; i < sizeof(P.s[k]); i++) z[i] = 0;
            bad++;
        }
    P.scanned = 1;
    sort();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "photos: ");
    ub_dec(&u, (uint64_t)P.n);
    ub_s(&u, " in \"" PH_NAME "\" at ");
    ub_s(&u, P.med == SB_MED_SD ? "sd:" : "ufs:");
    ub_dec(&u, P.e.lba);
    ub_s(&u, ", ");
    ub_dec(&u, (uint64_t)bad);
    ub_s(&u, " slots empty");
    sb_log(u.b);
    return 1;
}

/* writes refused while the modem runs (storage held) and in a safe start: 1 refused */
static int refused(void)
{
    if (sb_storage_held()) {
        fail("Storage is busy while the modem is on. Try again later.");
        return 1;
    }
    if (sbos_safe_start()) {
        fail("A safe start: nothing is saved until the next start.");
        return 1;
    }
    return 0;
}

/* the fence's grant for blocks first .. first + count - 1 of slot k (lent from the state slot) */
static int grant(int k, uint64_t first, uint64_t count)
{
    struct store_ent e;
    if (!store_volatile_grant_part(PH_NAME, PH_ENTRY, (uint64_t)k * PH_SLOT_BLOCKS + first, count, &e) ||
        e.lba != P.e.lba)
        return 0;
    hw_slot_lend();
    return 1;
}

static int wr(int k, uint64_t first, uint64_t blocks, const void *src)
{
    int r = sb_write_volatile(P.med, slot_lba(k) + first, blocks, src);
    if (r == SB_W_DRYRUN) return 1;             /* the fence's dry-run switch: logged by it, nothing written */
    return r ? -1 : 0;
}

void kphotos_fit(int w, int h, int *sw, int *sh)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((uint64_t)w * (uint64_t)h <= KPHOTO_MAX_PX) {
        *sw = w;
        *sh = h;
        return;
    }
    uint64_t lo = 1, hi = (uint64_t)w;          /* the widest that fits, its height rounded from the aspect */
    while (lo < hi) {
        uint64_t mid = (lo + hi + 1) / 2, dh = ((uint64_t)h * mid + (uint64_t)w / 2) / (uint64_t)w;
        if (!dh) dh = 1;
        if (mid * dh <= KPHOTO_MAX_PX) lo = mid;
        else hi = mid - 1;
    }
    uint64_t dh = ((uint64_t)h * lo + (uint64_t)w / 2) / (uint64_t)w;
    *sw = (int)lo;
    *sh = dh ? (int)dh : 1;
}

/* argb (w x h) as 24-bit RGB at sw x sh into o: copied, or each pixel the mean of its source box */
static void encode(const uint32_t *argb, int w, int h, int sw, int sh, uint8_t *o)
{
    if (sw == w && sh == h) {
        for (uint64_t i = 0, n = (uint64_t)w * (uint64_t)h; i < n; i++) {
            uint32_t c = argb[i];
            o[3 * i] = (uint8_t)(c >> 16);
            o[3 * i + 1] = (uint8_t)(c >> 8);
            o[3 * i + 2] = (uint8_t)c;
        }
        return;
    }
    for (int y = 0; y < sh; y++) {
        uint32_t y0 = (uint32_t)y * (uint32_t)h / (uint32_t)sh, y1 = (uint32_t)(y + 1) * (uint32_t)h / (uint32_t)sh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < sw; x++) {
            uint32_t x0 = (uint32_t)x * (uint32_t)w / (uint32_t)sw, x1 = (uint32_t)(x + 1) * (uint32_t)w / (uint32_t)sw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, c = (x1 - x0) * (y1 - y0);
            for (uint32_t yy = y0; yy < y1; yy++)
                for (uint32_t xx = x0; xx < x1; xx++) {
                    uint32_t v = argb[(uint64_t)yy * (uint32_t)w + xx];
                    r += v >> 16 & 255U;
                    g += v >> 8 & 255U;
                    b += v & 255U;
                }
            uint8_t *p = o + 3 * ((uint64_t)y * (uint32_t)sw + (uint32_t)x);
            p[0] = (uint8_t)((r + c / 2) / c);
            p[1] = (uint8_t)((g + c / 2) / c);
            p[2] = (uint8_t)((b + c / 2) / c);
        }
    }
}

int kphotos_save(const uint32_t *argb, int w, int h, int64_t utc, uint8_t *work, uint32_t cap)
{
    P.why = 0;
    if (!argb || w < 1 || h < 1 || w > KPHOTO_MAX_SIDE || h > KPHOTO_MAX_SIDE)
        return fail("This picture can't be saved (its size).");
    if (!work || cap < KPHOTO_WORK) return fail("No memory to save the photo.");
    if (refused() || !scan()) return -1;
    int sw, sh;
    kphotos_fit(w, h, &sw, &sh);
    uint32_t n = (uint32_t)sw * (uint32_t)sh * 3U, blocks = (n + 4095U) / 4096U;
    encode(argb, w, h, sw, sh, work);
    for (uint32_t i = n; i < blocks * 4096U; i++) work[i] = 0;
    int k = -1;
    uint64_t top = 0;
    for (int j = 0; j < KPHOTO_SLOTS; j++) {
        if (P.s[j].ok && P.s[j].seq > top) top = P.s[j].seq;
        if (k < 0 && !P.s[j].ok) k = j;         /* the first free slot */
    }
    if (k < 0) {                                /* full: the oldest */
        k = 0;
        for (int j = 1; j < KPHOTO_SLOTS; j++)
            if (P.s[j].seq < P.s[k].seq) k = j;
    }
    struct ph p;
    uint8_t *z = (uint8_t *)&p;
    for (unsigned i = 0; i < sizeof(p); i++) z[i] = 0;
    p.valid = p.ok = 1;
    p.seq = top + 1;
    p.w = (uint32_t)sw;
    p.h = (uint32_t)sh;
    p.fmt = 1;
    p.utc = utc;
    p.n = n;
    p.dcrc = sb_crc32(work, n);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, (uint64_t)sw);
    ub_c(&u, 'x');
    ub_dec(&u, (uint64_t)sh);
    ub_s(&u, " in slot ");
    ub_dec(&u, (uint64_t)k);
    if (dry()) {
        P.why = "A dry run: the self test.";
        sb_logf("photos: would save (a dry run, nothing written) ", u.b);
        return 0;
    }
    uint64_t t0 = sb_ms();
    if (!grant(k, 0, PH_SLOT_BLOCKS)) return fail("The photo storage could not be opened for writing.");
    int r = 0;
    if (P.s[k].valid) {                         /* the old header goes first: never a header over other pixels */
        for (unsigned i = 0; i < 4096U; i++) hb[i] = 0;
        r = wr(k, 0, 1, hb);
        P.s[k].valid = P.s[k].ok = 0;
        sort();
    }
    if (!r) r = wr(k, 1, blocks, work);
    if (!r && P.med != SB_MED_SD) {             /* UFS: forced unit access, no read-back in the fence */
        if (sb_blk_read(P.med, 0, slot_lba(k) + 1, blocks, work) || sb_crc32(work, n) != p.dcrc) r = -1;
    }
    if (!r) {
        hdr_build(hb, &p);
        r = wr(k, 0, 1, hb);
    }
    if (r > 0) {
        P.scanned = 0;
        P.why = "A dry run: nothing was written.";
        return 0;
    }
    if (!r && (sb_blk_read(P.med, 0, slot_lba(k), 1, rb) || !hdr_parse(rb, &P.s[k]) || P.s[k].seq != p.seq ||
               P.s[k].dcrc != p.dcrc))
        r = -1;
    if (r) {
        P.scanned = 0;                          /* read the headers again next time */
        return fail("The photo could not be saved (a storage error).");
    }
    sort();
    ub_s(&u, ", seq ");
    ub_dec(&u, p.seq);
    ub_s(&u, ", ");
    ub_dec(&u, sb_ms() - t0);
    ub_s(&u, " ms");
    sb_logf("photos: saved ", u.b);
    return 0;
}

int kphotos_count(void)
{
    return scan() ? P.n : 0;
}

int kphotos_get(int i, uint32_t *argb, int cap_px, int *w, int *h, int64_t *utc, uint8_t *work, uint32_t cap)
{
    P.why = 0;
    if (!scan()) return -1;
    for (;;) {
        if (i < 0 || i >= P.n) return fail("No such photo.");
        int k = P.order[i];
        const struct ph *s = &P.s[k];
        if (w) *w = (int)s->w;
        if (h) *h = (int)s->h;
        if (utc) *utc = s->utc;
        int px = (int)(s->w * s->h);
        if (!argb) return px;
        if (cap_px < px) return fail("The photo is larger than the space for it.");
        uint32_t blocks = (s->n + 4095U) / 4096U;
        if (!work || cap < blocks * 4096U) return fail("No memory to show the photo.");
        if (sb_blk_read(P.med, 0, slot_lba(k) + 1, blocks, work)) return fail("The photo could not be read.");
        if (sb_crc32(work, s->n) != s->dcrc) {  /* damaged (a torn write): skipped; the next one moves up */
            struct ub u;
            ub_init(&u);
            ub_s(&u, "photos: slot ");
            ub_dec(&u, (uint64_t)k);
            ub_s(&u, " is damaged (its pixels' CRC32): skipped");
            sb_log(u.b);
            P.s[k].ok = 0;
            sort();
            continue;
        }
        if (s->fmt == 1)
            for (int j = 0; j < px; j++)
                argb[j] = 0xff000000U | (uint32_t)work[3 * j] << 16 | (uint32_t)work[3 * j + 1] << 8 | work[3 * j + 2];
        else
            for (int j = 0; j < px; j++) {
                uint32_t v = (uint32_t)work[2 * j] | (uint32_t)work[2 * j + 1] << 8;
                uint32_t r = v >> 11 & 31U, g = v >> 5 & 63U, b = v & 31U;
                argb[j] = 0xff000000U | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        return px;
    }
}

int kphotos_delete(int i)
{
    P.why = 0;
    if (refused() || !scan()) return -1;
    if (i < 0 || i >= P.n) return fail("No such photo.");
    int k = P.order[i];
    struct ph p = P.s[k];
    struct ub u;
    ub_init(&u);
    ub_s(&u, "slot ");
    ub_dec(&u, (uint64_t)k);
    ub_s(&u, ", seq ");
    ub_dec(&u, p.seq);
    if (dry()) {
        P.why = "A dry run: the self test.";
        sb_logf("photos: would delete (a dry run, nothing written) ", u.b);
        return 0;
    }
    if (!grant(k, 0, 1)) return fail("The photo storage could not be opened for writing.");
    p.seq = 0;
    hdr_build(hb, &p);
    int r = wr(k, 0, 1, hb);
    if (r > 0) {
        P.why = "A dry run: nothing was written.";
        return 0;
    }
    if (r || sb_blk_read(P.med, 0, slot_lba(k), 1, rb) || !hdr_parse(rb, &P.s[k]) || P.s[k].ok) {
        P.scanned = 0;
        return fail("The photo could not be deleted (a storage error).");
    }
    sort();
    sb_logf("photos: deleted ", u.b);
    return 0;
}

void kphotos_reset(void)
{
    uint8_t *z = (uint8_t *)&P;
    for (unsigned i = 0; i < sizeof(P); i++) z[i] = 0;
}
