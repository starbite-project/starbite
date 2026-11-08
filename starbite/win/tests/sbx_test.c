/* SPDX-License-Identifier: GPL-2.0-only
 * Host test for sbwddm/sbx_cmd.c: every rule of the UMD -> KMD ABI, plus fuzzing under AddressSanitizer
 * with buffers allocated at their exact length (any read past `len` aborts the test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sbwddm/sbx_cmd.h"

static int fails, checks;
#define CHECK(c, ...)                                                                    \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(c)) {                                                                      \
            ++fails;                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

static unsigned seed = 1;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

/* Run the parser on an exact-length heap copy. */
static int parse(const void *b, unsigned len, const struct sbx_alloc_ref *a, unsigned n, struct sbx_job_out *o)
{
    unsigned char *c = malloc(len ? len : 1);
    int r;
    memcpy(c, b, len);
    r = sbx_cmd_parse(c, len, a, n, o, SBX_MAX_JOBS);
    free(c);
    return r;
}

static int acheck(const struct sbx_alloc *s, unsigned len)
{
    unsigned char *c = malloc(len ? len : 1);
    int r;
    memcpy(c, s, len < sizeof(*s) ? len : sizeof(*s));
    r = sbx_alloc_check(c, len);
    free(c);
    return r;
}

int main(void)
{
    struct sbx_alloc_ref al[3] = { { 0x02000000, 0x10000, SBX_KIND_BUFFER, 0 },
                                   { 0x10000000, 0x1000000, SBX_KIND_HEAP, SBX_ALLOC_NOEXEC },
                                   { 0x03000000, 0x1000, SBX_KIND_BUFFER, 0 } };
    struct sbx_job_out o[SBX_MAX_JOBS];
    struct { struct sbx_cmd h; struct sbx_job j[SBX_MAX_JOBS]; } c;
    struct sbx_alloc s;
    unsigned len;

    /* structure sizes are the ABI */
    CHECK(sizeof(struct sbx_alloc) == 32 && sizeof(struct sbx_cmd) == 8 && sizeof(struct sbx_job) == 16, "ABI sizes");

    /* allocations */
    s = (struct sbx_alloc){ SBX_ABI_VERSION, SBX_KIND_BUFFER, SBX_ALLOC_CPU, 0, 0x4000, 0 };
    CHECK(acheck(&s, sizeof(s)) == 0, "valid allocation");
    CHECK(acheck(&s, sizeof(s) - 1) == SBX_E_SHORT, "short private data");
    s.version = 2;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_VERSION, "version");
    s.version = SBX_ABI_VERSION;
    s.kind = 0;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_KIND, "kind 0");
    s.kind = 7;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_KIND, "kind 7");
    s.kind = SBX_KIND_PRIMARY;
    s.flags = 0x8;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_FLAGS, "unknown flag");
    s.flags = 0;
    s.reserved = 1;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_FLAGS, "reserved non-zero");
    s.reserved = 0;
    s.size = 0x4001;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_SIZE, "size not page multiple");
    s.size = 0;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_SIZE, "size 0");
    s.size = (1ULL << 31) + 0x1000;
    CHECK(acheck(&s, sizeof(s)) == SBX_E_SIZE, "size over 2 GiB");
    CHECK(sbx_alloc_check(NULL, 32) == SBX_E_SHORT, "NULL");

    /* commands */
    memset(&c, 0, sizeof(c));
    c.h.magic = SBX_CMD_MAGIC;
    c.h.count = 2;
    c.j[0] = (struct sbx_job){ 1, 0, 0x40 };
    c.j[1] = (struct sbx_job){ 0, 2, 0x1000 - 64 };
    len = 8 + 2 * 16;
    CHECK(parse(&c, len, al, 3, o) == 2 && o[0].slot == 1 && o[0].head == 0x02000040 && o[1].slot == 0 &&
              o[1].head == 0x03000fc0, "two valid jobs");
    CHECK(parse(&c, len - 1, al, 3, o) == SBX_E_SHORT, "one byte short");
    CHECK(parse(&c, 7, al, 3, o) == SBX_E_SHORT, "header short");
    c.h.magic ^= 1;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_MAGIC, "magic");
    c.h.magic ^= 1;
    c.h.count = 0;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_COUNT, "count 0");
    c.h.count = SBX_MAX_JOBS + 1;
    CHECK(parse(&c, sizeof(c), al, 3, o) == SBX_E_COUNT, "count over the maximum");
    c.h.count = 0xffffffffu;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_COUNT, "count huge");
    c.h.count = 2;
    c.j[0].slot = 3;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_SLOT, "slot 3");
    c.j[0].slot = 1;
    c.j[0].alloc_index = 3;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_INDEX, "index past the list");
    CHECK(parse(&c, len, NULL, 0, o) == SBX_E_INDEX, "empty allocation list");
    c.j[0].alloc_index = 1;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_KIND, "chain in the tiler heap refused");
    c.j[0].alloc_index = 0;
    c.j[0].offset = 0x41;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_ALIGN, "unaligned");
    c.j[0].offset = 0x10000 - 32;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_ALIGN, "half a descriptor (unaligned)");
    c.j[0].offset = 0x10000;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_RANGE, "at the end");
    c.j[0].offset = 0xffffffffffffffc0ULL;
    CHECK(parse(&c, len, al, 3, o) == SBX_E_RANGE, "offset that would wrap");
    c.j[0].offset = 0x10000 - 64;
    CHECK(parse(&c, len, al, 3, o) == 2 && o[0].head == 0x0200ffc0, "last whole descriptor");

    /* sbx_head_in: the range rule on its own */
    CHECK(sbx_head_in(0x1000, 0x1000, 0x40) && !sbx_head_in(0x1040, 0x1000, 0x40) && !sbx_head_in(0x1020, 0x1000, 0x1000) &&
          !sbx_head_in(0xfc0, 0x1000, 0x1000) && sbx_head_in(0x1fc0, 0x1000, 0x1000) && !sbx_head_in(0x2000, 0x1000, 0x1000) &&
          !sbx_head_in(0x1000, 0x1000, 0x20) && !sbx_head_in(0xffffffffffffffc0ULL, 0xffffffffffff0000ULL, 0x20000),
          "head range rule: aligned, whole descriptor inside, no wrap");

    /* fuzz: random buffers and single-byte mutations of a valid one; must never read past len, and every
     * accepted job must lie inside a buffer allocation */
    {
        unsigned char b[8 + 16 * SBX_MAX_JOBS + 16];
        int accepted = 0, bad = 0;
        for (int k = 0; k < 200000; ++k) {
            unsigned l = rnd() % sizeof(b);
            int r;
            if (k & 1) {
                for (unsigned i = 0; i < l; ++i)
                    b[i] = (unsigned char)rnd();
                if (l >= 4 && (k & 2)) {
                    b[0] = 0x53; b[1] = 0x42; b[2] = 0x58; b[3] = 0x31;
                }
            } else {
                memcpy(b, &c, sizeof(b) < sizeof(c) ? sizeof(b) : sizeof(c));
                l = len;
                b[rnd() % l] ^= (unsigned char)(1u << (rnd() % 8));
            }
            r = parse(b, l, al, 3, o);
            if (r > 0) {
                accepted++;
                for (int i = 0; i < r; ++i) {
                    int in = 0;
                    for (int a = 0; a < 3; ++a)
                        in |= al[a].kind == SBX_KIND_BUFFER && o[i].head >= al[a].gpu_va &&
                              o[i].head + 64 <= al[a].gpu_va + al[a].size && !(o[i].head & 63);
                    bad += !in || o[i].slot > 2;
                }
            }
        }
        CHECK(!bad && accepted > 0, "fuzz: 200000 buffers, %d accepted, %d accepted jobs outside a buffer", accepted, bad);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
