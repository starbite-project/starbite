/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/photos.c (Track A: the Camera's photo store) with libsb's real str.c (CRC32, the
 * log's text), around fakes: the home store's volatile entry "sbos-photos" (64 MiB in RAM), the fence's one grant
 * (every write must fall inside it; the state slot's lend counted), the storage hold, a safe start, power losses
 * (a write that fails part way) and read errors.
 * Checks: the missing entry (why), save / get round trips in both orientations (exact pixels, w, h, utc), the size
 * query, a picture scaled down to fit (1080 x 1440 -> 723 x 964, a box mean), the write order (an old header made
 * invalid first, the pixels, the header last, all inside the slot's grant), a restart (the headers read again), the
 * ring's wrap after 32 (the oldest goes, newest first), delete, a torn write (a header over pixels whose CRC32 does
 * not match: skipped, the count drops), a torn header, a power loss before the header (nothing half there), the
 * storage hold and the safe start (refused, nothing written), the dry run, UFS (the pixels read back), an RGB565
 * slot read.
 * Run: sh starbite/sbos/kern/host/photos-test.sh [-v]. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"
#include "store.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        } else if (verbose) {                                                                                          \
            printf("ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- libsb's host bits ---------------------------------------------------------------------------------------- */
uint8_t sb_host_dma[1u << 16] __attribute__((aligned(4096)));
uint8_t sb_host_fw_page[4096] __attribute__((aligned(4096)));
const int sbos_selftest_enabled = 0;
uint64_t sb_ticks(void) { return 1; }
uint64_t sb_ms(void) { return 1000; }
void sb_log(const char *s) { if (verbose) printf("    log: %s\n", s); }
void sb_logf(const char *a, const char *b) { if (verbose) printf("    log: %s%s\n", a, b); }
void sb_logx(const char *a, uint64_t v) { if (verbose) printf("    log: %s0x%llx\n", a, (unsigned long long)v); }

/* ---- the home store: "sbos-photos" --------------------------------------------------------------------------- */
#define ENT_BYTES (64u << 20)
#define ENT_LBA 20000u
#define SLOT_BLK 512u
static uint8_t *mem;                            /* the entry's bytes */
static int have_ent = 1, ent_med = STORE_SD;
static uint64_t ent_size = ENT_BYTES;
static const char *held;
static int safe, lends, grants, wr_refused, rd_fail;
static uint64_t g_first, g_n;
static int g_on;
static int fail_write = -1;                     /* the index (in wlog) of a write that fails: the power is cut */
static struct { uint64_t lba, n; int zero_hdr; } wlog[256];
static int nw;

int store_home(void) { return 1; }
int sbos_safe_start(void) { return safe; }
const char *sb_storage_held(void) { return held; }
void hw_slot_lend(void) { lends++; }
int store_find(const char *name, struct store_ent *e)
{
    if (!have_ent || strcmp(name, "home:sbos-photos")) return 0;
    memset(e, 0, sizeof(*e));
    strcpy(e->name, "sbos-photos");
    e->lba = ENT_LBA;
    e->size = ent_size;
    e->flags = 1;
    e->medium = (uint8_t)ent_med;
    return 1;
}
int store_volatile_grant_part(const char *name, uint64_t min_bytes, uint64_t first, uint64_t count, struct store_ent *e)
{
    g_on = 0;
    if (strcmp(name, "sbos-photos") || !store_find("home:sbos-photos", e) || e->size < min_bytes) return 0;
    uint64_t n = e->size / 4096;
    if (first >= n || !count || count > 1024) return 0;
    if (count > n - first) count = n - first;
    g_first = e->lba + first;
    g_n = count;
    g_on = 1;
    grants++;
    return 1;
}
static uint8_t *at(uint64_t lba)
{
    return lba >= ENT_LBA && lba < ENT_LBA + ENT_BYTES / 4096 ? mem + (lba - ENT_LBA) * 4096 : 0;
}
int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst)
{
    (void)lu;
    if (medium != (ent_med == STORE_SD ? SB_MED_SD : SB_MED_UFS) || rd_fail) return -1;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t *p = at(lba + i);
        if (!p) return -1;
        memcpy((uint8_t *)dst + i * 4096, p, 4096);
    }
    return 0;
}
static int fence_dry;
int sb_write_volatile(unsigned medium, uint64_t lba, uint64_t n, const void *src)
{
    if (!g_on || medium != (ent_med == STORE_SD ? SB_MED_SD : SB_MED_UFS) || lba < g_first || lba + n > g_first + g_n) {
        wr_refused++;
        return SB_W_RANGE;
    }
    if (fence_dry) return SB_W_DRYRUN;
    if (nw == fail_write) {                     /* the power goes: half the blocks written */
        for (uint64_t i = 0; i < n / 2; i++) memcpy(at(lba + i), (const uint8_t *)src + i * 4096, 4096);
        nw++;
        return SB_W_IO;
    }
    for (uint64_t i = 0; i < n; i++) memcpy(at(lba + i), (const uint8_t *)src + i * 4096, 4096);
    if (nw < 256) {
        wlog[nw].lba = lba - ENT_LBA;
        wlog[nw].n = n;
        int z = 1;
        for (int i = 0; i < 64; i++) z &= ((const uint8_t *)src)[i] == 0;
        wlog[nw].zero_hdr = z;
    }
    nw++;
    return 0;
}

/* ---- helpers ------------------------------------------------------------------------------------------------------ */
static uint8_t work[KPHOTO_WORK];
static uint32_t pix(int seed, int x, int y) { return 0xff000000u | ((uint32_t)(x * 7 + seed * 13) & 255) << 16 | ((uint32_t)(y * 5 + seed) & 255) << 8 | ((uint32_t)(x ^ y ^ seed) & 255); }
static uint32_t *picture(int w, int h, int seed)
{
    uint32_t *p = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) p[y * w + x] = pix(seed, x, y) & (seed & 1 ? 0xffffffffu : 0x00ffffffu);   /* alpha ignored */
    return p;
}
static int save(int w, int h, int seed, int64_t utc)
{
    uint32_t *p = picture(w, h, seed);
    int r = kphotos_save(p, w, h, utc, work, sizeof(work));
    free(p);
    return r;
}
/* photo i is the picture (w, h, seed, utc) */
static int same(int i, int w, int h, int seed, int64_t utc)
{
    static uint32_t out[KPHOTO_MAX_PX];
    int gw = 0, gh = 0;
    int64_t gu = 0;
    int n = kphotos_get(i, out, KPHOTO_MAX_PX, &gw, &gh, &gu, work, sizeof(work));
    if (n != w * h || gw != w || gh != h || gu != utc) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (out[y * w + x] != (pix(seed, x, y) | 0xff000000u)) return 0;
    return 1;
}
static int64_t utc_of(int i)
{
    int64_t u = -1;
    int w, h;
    return kphotos_get(i, 0, 0, &w, &h, &u, 0, 0) > 0 ? u : -1;
}
static void restart(void) { kphotos_reset(); }
static void fresh(void)
{
    memset(mem, 0, ENT_BYTES);
    kphotos_reset();
    kphotos_dry(0);
    have_ent = 1;
    ent_size = ENT_BYTES;
    ent_med = STORE_SD;
    held = 0;
    safe = 0;
    nw = 0;
    fail_write = -1;
    wr_refused = 0;
    rd_fail = 0;
    fence_dry = 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    mem = calloc(1, ENT_BYTES);
    int sw, sh;

    /* -- the sizes kept */
    kphotos_fit(612, 816, &sw, &sh);
    CHECK(sw == 612 && sh == 816, "612 x 816 kept as it is (%dx%d)", sw, sh);
    kphotos_fit(1080, 1440, &sw, &sh);
    CHECK(sw == 723 && sh == 964 && sw * sh <= (int)KPHOTO_MAX_PX, "1080 x 1440 kept as 723 x 964 (%dx%d)", sw, sh);
    kphotos_fit(1440, 1080, &sw, &sh);
    CHECK(sw == 964 && sh == 723, "1440 x 1080 kept as 964 x 723 (%dx%d)", sw, sh);
    kphotos_fit(4096, 4096, &sw, &sh);
    CHECK(sw * sh <= (int)KPHOTO_MAX_PX && sw == sh && sw > 830, "4096 x 4096 kept square under the cap (%dx%d)", sw, sh);
    kphotos_fit(4096, 1, &sw, &sh);
    CHECK(sw == 4096 && sh == 1, "a 4096 x 1 strip kept (%dx%d)", sw, sh);
    CHECK((uint64_t)KPHOTO_MAX_PX * 3 <= KPHOTO_SLOT_BYTES - 4096, "the largest photo fits a slot after its header");

    /* -- no entry */
    fresh();
    have_ent = 0;
    CHECK(kphotos_count() == 0, "no entry: count 0");
    CHECK(save(10, 10, 1, 5) == -1 && strstr(kphotos_why(), "sbos-photos"), "no entry: save refused: %s", kphotos_why());
    CHECK(kphotos_get(0, 0, 0, 0, 0, 0, 0, 0) == -1, "no entry: get -1");
    CHECK(kphotos_delete(0) == -1, "no entry: delete -1");
    CHECK(nw == 0 && grants == 0, "no entry: nothing granted or written");
    fresh();
    ent_size = 32u << 20;
    CHECK(save(10, 10, 1, 5) == -1 && strstr(kphotos_why(), "sbos-photos"), "an entry under 64 MiB: refused");

    /* -- round trips */
    fresh();
    CHECK(kphotos_count() == 0, "an empty entry: count 0");
    int l0 = lends;
    CHECK(save(612, 816, 3, 1762900000) == 0, "portrait 612 x 816 saved (%s)", kphotos_why());
    CHECK(kphotos_count() == 1, "count 1");
    CHECK(lends > l0 && wr_refused == 0, "the grant lent from the state slot, every write inside it");
    CHECK(nw == 2 && wlog[0].lba == 1 && wlog[0].n == (612u * 816 * 3 + 4095) / 4096 && wlog[1].lba == 0 && wlog[1].n == 1,
          "a free slot: the pixels (blocks 1..%llu) then the header last", (unsigned long long)wlog[0].n);
    CHECK(same(0, 612, 816, 3, 1762900000), "portrait read back exactly (pixels, size, utc)");
    CHECK(save(816, 612, 4, 1762900060) == 0, "landscape 816 x 612 saved");
    CHECK(kphotos_count() == 2 && same(0, 816, 612, 4, 1762900060) && same(1, 612, 816, 3, 1762900000),
          "newest first: landscape, then portrait");
    CHECK(wlog[2].lba == SLOT_BLK + 1 && wlog[3].lba == SLOT_BLK, "the second photo in slot 1, header last");
    int w = 0, h = 0;
    int64_t u = 0;
    CHECK(kphotos_get(1, 0, 0, &w, &h, &u, 0, 0) == 612 * 816 && w == 612 && h == 816 && u == 1762900000,
          "argb 0: the size and time only");
    static uint32_t small[100];
    CHECK(kphotos_get(0, small, 100, &w, &h, &u, work, sizeof(work)) == -1 && w == 816, "a buffer too small: -1 (%s)", kphotos_why());
    CHECK(kphotos_get(0, small, 100000000, &w, &h, &u, work, 4096) == -1, "a work buffer too small: -1");
    CHECK(kphotos_get(2, 0, 0, 0, 0, 0, 0, 0) == -1 && kphotos_get(-1, 0, 0, 0, 0, 0, 0, 0) == -1, "out of range: -1");
    CHECK(kphotos_save(small, 0, 10, 0, work, sizeof(work)) == -1 && kphotos_save(small, 5000, 1, 0, work, sizeof(work)) == -1,
          "a bad size refused");
    CHECK(kphotos_save(small, 10, 10, 0, work, 1000) == -1, "no work buffer: refused");

    /* a restart: the headers read again */
    restart();
    CHECK(kphotos_count() == 2 && same(0, 816, 612, 4, 1762900060) && same(1, 612, 816, 3, 1762900000), "after a restart: both, newest first");

    /* -- scaled down: 1080 x 1440 of 2 x 2-ish blocks of one colour, so the box mean is checkable */
    {
        int W = 1080, H = 1440;
        uint32_t *p = malloc((size_t)W * H * 4);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) p[y * W + x] = 0xff000000u | (x < W / 2 ? 0x204060u : 0xa0c0e0u);
        p[2 * W + 2] = 0xff000000u;             /* box (2, 2) is pixels 2..3 x 2..3: one black, three 0x204060 */
        CHECK(kphotos_save(p, W, H, 0, work, sizeof(work)) == 0, "1080 x 1440 saved");
        static uint32_t out[KPHOTO_MAX_PX];
        int n = kphotos_get(0, out, KPHOTO_MAX_PX, &w, &h, &u, work, sizeof(work));
        CHECK(n == 723 * 964 && w == 723 && h == 964, "kept as 723 x 964 (%dx%d)", w, h);
        CHECK(out[964 / 2 * 723 + 10] == 0xff204060u && out[964 / 2 * 723 + 700] == 0xffa0c0e0u, "flat areas exact");
        uint32_t c = out[2 * 723 + 2];
        CHECK(c == 0xff183048u && out[0] == 0xff204060u, "a box's mean, rounded (%08x)", c);
        free(p);
    }

    /* -- the ring: 40 saves, the newest 32 kept */
    fresh();
    for (int i = 0; i < 40; i++) {
        int pw = 30 + i, ph = 20 + (i % 7);
        if (save(pw, ph, i, 1000 + i)) { CHECK(0, "save %d (%s)", i, kphotos_why()); break; }
        if (i == 32) {                          /* the first wrap: slot 0's old header made invalid first */
            CHECK(nw >= 3 && wlog[nw - 3].lba == 0 && wlog[nw - 3].n == 1 && wlog[nw - 3].zero_hdr &&
                  wlog[nw - 2].lba == 1 && wlog[nw - 1].lba == 0 && !wlog[nw - 1].zero_hdr,
                  "a full ring: the oldest slot's header zeroed, the pixels, then the new header");
        }
    }
    CHECK(kphotos_count() == 32, "32 kept (%d)", kphotos_count());
    int ok = 1;
    for (int i = 0; i < 32; i++) ok &= utc_of(i) == 1039 - i;
    CHECK(ok, "newest first: saves 39 .. 8");
    CHECK(same(0, 69, 20 + 39 % 7, 39, 1039) && same(31, 38, 20 + 8 % 7, 8, 1008), "the newest and the oldest read back exactly");
    restart();
    ok = kphotos_count() == 32;
    for (int i = 0; i < 32; i++) ok &= utc_of(i) == 1039 - i;
    CHECK(ok, "after a restart: the same 32 in the same order");
    CHECK(wr_refused == 0, "every write inside its grant");

    /* -- delete */
    nw = 0;
    CHECK(kphotos_delete(0) == 0 && kphotos_count() == 31 && utc_of(0) == 1038, "delete the newest: 31, 38 first");
    CHECK(nw == 1 && wlog[0].n == 1, "a delete: one header block written");
    CHECK(kphotos_delete(10) == 0 && kphotos_count() == 30 && utc_of(9) == 1029 && utc_of(10) == 1027, "delete the 11th: the next moves up");
    CHECK(kphotos_delete(30) == -1, "delete out of range: -1");
    restart();
    CHECK(kphotos_count() == 30 && utc_of(0) == 1038 && utc_of(10) == 1027, "after a restart: still 30");
    CHECK(save(5, 5, 77, 2000) == 0 && kphotos_count() == 31 && utc_of(0) == 2000 && same(0, 5, 5, 77, 2000), "a save fills a deleted slot");
    CHECK(utc_of(30) == 1008, "the oldest kept while a slot was free");

    /* -- a torn write: a header over pixels that are not its own */
    fresh();
    for (int i = 0; i < 4; i++) save(40, 30, i, 100 + i);
    mem[1 * SLOT_BLK * 4096 + 4096 + 77] ^= 0x5a;   /* slot 1 (utc 101)'s pixels */
    restart();
    CHECK(kphotos_count() == 4, "a torn photo's header still counts before its pixels are read");
    CHECK(same(0, 40, 30, 3, 103) && same(1, 40, 30, 2, 102), "the two newest read");
    CHECK(same(2, 40, 30, 0, 100), "index 2: the torn photo skipped, the next one given");
    CHECK(kphotos_count() == 3, "the count drops to 3");
    CHECK(save(40, 30, 9, 109) == 0 && utc_of(0) == 109 && kphotos_count() == 4, "a save reuses the torn slot");
    CHECK(wlog[nw - 3].lba == SLOT_BLK && wlog[nw - 3].zero_hdr, "the torn slot's header zeroed first");
    /* a torn header */
    mem[2 * SLOT_BLK * 4096 + 20] ^= 1;         /* slot 2 (utc 102)'s seq */
    restart();
    CHECK(kphotos_count() == 3 && utc_of(0) == 109 && utc_of(1) == 103 && utc_of(2) == 100, "a header whose CRC is wrong: not a photo");

    /* -- the power goes: the pixels' write of a wrap, the header's write */
    fresh();
    for (int i = 0; i < 32; i++) save(20, 20, i, 300 + i);
    fail_write = nw + 1;                        /* the slot 0 header zeroed, then the pixels cut */
    CHECK(save(20, 20, 50, 400) == -1 && strstr(kphotos_why(), "storage error"), "the pixels' write cut: -1 (%s)", kphotos_why());
    restart();
    CHECK(kphotos_count() == 31 && utc_of(0) == 331 && utc_of(30) == 301, "after the cut: 31 whole photos, the oldest gone, nothing torn");
    fail_write = nw + 1;                        /* a free slot: the pixels, then the header cut */
    CHECK(save(20, 20, 51, 401) == -1, "the header's write cut: -1");
    restart();
    CHECK(kphotos_count() == 31 && utc_of(0) == 331, "a cut header: no photo there");
    fail_write = -1;
    CHECK(save(20, 20, 52, 402) == 0 && kphotos_count() == 32 && same(0, 20, 20, 52, 402), "the next save is whole");

    /* -- read errors */
    rd_fail = 1;
    static uint32_t out2[400];
    CHECK(kphotos_get(0, out2, 400, &w, &h, &u, work, sizeof(work)) == -1 && strstr(kphotos_why(), "read"), "a read error: -1 (%s)", kphotos_why());
    rd_fail = 0;
    CHECK(kphotos_count() == 32, "a read error drops nothing");

    /* -- the storage hold, the safe start, the dry runs */
    fresh();
    save(8, 8, 1, 1);
    int nw0 = nw;
    held = "modem";
    CHECK(save(8, 8, 2, 2) == -1 && strstr(kphotos_why(), "modem"), "storage held: save refused (%s)", kphotos_why());
    CHECK(kphotos_delete(0) == -1 && strstr(kphotos_why(), "modem"), "storage held: delete refused");
    CHECK(kphotos_count() == 1 && same(0, 8, 8, 1, 1), "storage held: reads still work");
    held = 0;
    safe = 1;
    CHECK(save(8, 8, 2, 2) == -1 && strstr(kphotos_why(), "safe start"), "a safe start: save refused (%s)", kphotos_why());
    CHECK(kphotos_delete(0) == -1, "a safe start: delete refused");
    safe = 0;
    CHECK(nw == nw0, "nothing written while held or in a safe start");
    kphotos_dry(1);
    CHECK(save(8, 8, 3, 3) == 0 && kphotos_delete(0) == 0 && nw == nw0 && kphotos_count() == 1, "the dry run: 0, nothing written");
    kphotos_dry(0);
    fence_dry = 1;
    CHECK(save(8, 8, 3, 3) == 0 && strstr(kphotos_why(), "dry run"), "the fence's dry run: 0, said");
    fence_dry = 0;
    restart();
    CHECK(kphotos_count() == 1 && same(0, 8, 8, 1, 1), "the dry runs changed nothing");

    /* -- UFS (the pixels read back by photos.c) */
    fresh();
    ent_med = STORE_UFS;
    CHECK(save(612, 816, 6, 6) == 0 && same(0, 612, 816, 6, 6), "UFS: saved, read back");
    restart();
    CHECK(kphotos_count() == 1 && same(0, 612, 816, 6, 6), "UFS: after a restart");

    /* -- an RGB565 slot (format 0) */
    fresh();
    {
        uint8_t *s = mem + 3 * SLOT_BLK * 4096, *d = s + 4096;
        uint16_t px[4] = { 0xf800, 0x07e0, 0x001f, 0xffff };
        for (int i = 0; i < 4; i++) { d[2 * i] = (uint8_t)px[i]; d[2 * i + 1] = (uint8_t)(px[i] >> 8); }
        uint8_t hdr[64] = "SBPHOTO1";
        hdr[8] = 1;
        hdr[16] = 5;                            /* seq 5 */
        hdr[24] = 2; hdr[28] = 2;               /* 2 x 2 */
        hdr[32] = 0;                            /* RGB565 */
        hdr[36] = 42;                           /* utc */
        uint32_t dc = sb_crc32(d, 8);
        for (int i = 0; i < 4; i++) hdr[44 + i] = (uint8_t)(dc >> (8 * i));
        hdr[48] = 8;
        uint32_t hc = sb_crc32(hdr, 64);
        for (int i = 0; i < 4; i++) hdr[12 + i] = (uint8_t)(hc >> (8 * i));
        memcpy(s, hdr, 64);
        uint32_t o[4];
        CHECK(kphotos_get(0, o, 4, &w, &h, &u, work, sizeof(work)) == 4 && w == 2 && h == 2 && u == 42 &&
              o[0] == 0xffff0000u && o[1] == 0xff00ff00u && o[2] == 0xff0000ffu && o[3] == 0xffffffffu,
              "an RGB565 slot read (%08x %08x %08x %08x)", o[0], o[1], o[2], o[3]);
        CHECK(save(3, 3, 1, 43) == 0 && utc_of(0) == 43 && utc_of(1) == 42, "a save after it: seq 6, newest");
    }

    printf("photos-test: %d checks, %d failed\n", checks, fails);
    free(mem);
    return fails ? 1 : 0;
}
