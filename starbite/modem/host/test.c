/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm host test: the core against golden values from this phone's logs and RADIO image, and against the fake
 * CP. Usage: sbm-test [RADIO.img]   (RADIO is read, never written; only its first 1 KB and its size are used)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakecp.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKEQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { fails++; printf("FAIL %s:%d: %s = %lld (0x%llx), want %lld (0x%llx)\n", __FILE__, __LINE__, \
        #a, _a, (unsigned long long)_a, _b, (unsigned long long)_b); } } while (0)

static char lastlog[512];
static char loglines[256][256];
static unsigned nlog;
static void tlog(void *ctx, const char *l)
{
    (void)ctx;
    snprintf(loglines[nlog % 256], sizeof(loglines[0]), "%s", l);
    nlog++;
    snprintf(lastlog, sizeof(lastlog), "%s", l);
    if (getenv("SBM_VERBOSE"))
        printf("  log: %s\n", l);
}

/* ---- TOC / memory map ---------------------------------------------------------------------- */

static void toc_ent(uint8_t *b, unsigned i, const char *name, uint32_t off, uint32_t load, uint32_t size, uint32_t id)
{
    uint8_t *r = b + 32 * i;
    memset(r, 0, 32);
    memcpy(r, name, strlen(name));
    sbm_put32(r + 12, off);
    sbm_put32(r + 16, load);
    sbm_put32(r + 20, size);
    sbm_put32(r + 24, 0x12345678);
    sbm_put32(r + 28, id);
}

/* this phone's TOC (values from backup/20261105-194211/RADIO.img and cbd's log) */
static void make_toc(uint8_t *b)
{
    memset(b, 0, 1024);
    toc_ent(b, 0, "TOC", 0, 0, 0x200, 5);
    toc_ent(b, 1, "BOOT", 0x200, 0x40000000, 0x1e40, 1);
    toc_ent(b, 2, "MAIN", 0x2040, 0x40010000, 0x23d8ea0, 2);
    toc_ent(b, 3, "VSS", 0x23daee0, 0x47900000, 0x57cbf0, 3);
    toc_ent(b, 4, "NV", 0, 0x44900000, 0x100000, 4);
    toc_ent(b, 5, "OFFSET", 0x7aa00, 0, 0x85600, 5);
}

/* this phone's MEM0 table, in the byte order found in its RADIO at 0x2a0 */
static void make_memmap(uint8_t *p)
{
    static const struct { const char rev[4]; uint32_t a, s; } bins[5] = {
        { "SSV", 0x07900000, 0x00700000 }, { "CPI", 0x08000000, 0x00500000 }, { "CMZ", 0x08500000, 0x00B00000 },
        { "APV", 0x09000000, 0x00200000 }, { "GOL", 0x09200000, 0x02000000 },
    };
    memset(p, 0, SBM_MEMMAP_BYTES);
    p[0] = 0; p[1] = 'M'; p[2] = 'E'; p[3] = 'M';
    sbm_put32(p + 4, 0x07900000);
    sbm_put32(p + 8, 0x01900000);
    sbm_put32(p + 12, 5);
    for (int i = 0; i < 5; ++i) {
        memcpy(p + 16 + 12 * i, bins[i].rev, 4);
        sbm_put32(p + 20 + 12 * i, bins[i].a);
        sbm_put32(p + 24 + 12 * i, bins[i].s);
    }
}

static void check_phone_map(const struct sbm_memmap *m)
{
    CHECK(m->valid);
    CHECK(!strcmp(m->id, "MEM0"));
    CHECKEQ(m->cp_size, 0x07900000);
    CHECKEQ(m->ext_mem_size, 0x01900000);
    CHECKEQ(m->ipc_off, 0x08000000);
    CHECKEQ(m->ipc_size, 0x00500000);
    CHECKEQ(m->vss_off, 0x07900000);
    CHECKEQ(m->vss_size, 0x00700000);
    CHECKEQ(m->zmb_off, 0x08500000);
    CHECKEQ(m->zmb_size, 0x00B00000);
    CHECKEQ(m->vparam_off, 0x09000000);
    CHECKEQ(m->vparam_size, 0x00200000);
    CHECKEQ(m->log_off, 0x09200000);
    CHECKEQ(m->log_size, 0x02000000);
    CHECK(!strcmp(m->bin[0].tag, "VSS") && !strcmp(m->bin[1].tag, "IPC") && !strcmp(m->bin[4].tag, "LOG"));
}

static void check_phone_plan(const struct sbm_toc *t, const struct sbm_memmap *m)
{
    struct sbm_plan p;
    CHECKEQ(sbm_plan_build(t, m, &p), 0);
    CHECKEQ(p.n, 4);
    CHECK(!strcmp(p.l[0].name, "BOOT") && !strcmp(p.l[1].name, "MAIN") && !strcmp(p.l[2].name, "VSS") &&
          !strcmp(p.l[3].name, "NV"));
    CHECKEQ(p.l[0].dst_off, 0);
    CHECKEQ(p.l[1].dst_off, 0x10000);
    CHECKEQ(p.l[2].dst_off, 0x7900000);
    CHECKEQ(p.l[3].dst_off, 0x4900000);
    CHECKEQ(p.l[3].src, SBM_SRC_NV);
    CHECKEQ(p.l[1].src_off, 0x2040);
    CHECKEQ(p.boot_size, 0x1e40);       /* cbd's security request: 0:1e40:23d8ea0:0 */
    CHECKEQ(p.main_size, 0x23d8ea0);
}

static void test_toc(void)
{
    uint8_t b[1024];
    struct sbm_toc t;
    struct sbm_memmap m;
    make_toc(b);
    CHECKEQ(sbm_toc_parse(b, 52428800, &t), 0);
    CHECKEQ(t.n, 6);
    CHECKEQ(t.hdr_size, 0x200);
    CHECK(t.boot == 1 && t.main == 2 && t.vss == 3 && t.nv == 4 && t.offset_ent == 5);
    CHECK(!strcmp(t.e[2].name, "MAIN"));
    make_memmap(b + SBM_MEMMAP_OFF);
    sbm_memmap_parse(b + SBM_MEMMAP_OFF, &m);
    check_phone_map(&m);
    check_phone_plan(&t, &m);

    /* refusals */
    make_toc(b);
    CHECKEQ(sbm_toc_parse(b, 0x1000000, &t), SBM_ERANGE);            /* MAIN beyond a 16 MB image */
    make_toc(b);
    b[32 * 2 + 1] = 0x01;                                            /* control char in a name */
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_EINVAL);
    make_toc(b);
    sbm_put32(b + 32 * 2 + 12, 0x1000);                              /* MAIN overlaps BOOT */
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_ERANGE);
    make_toc(b);
    memcpy(b + 32 * 2, "MAIX", 4);
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_ENOENT);
    make_toc(b);
    memcpy(b, "TOD", 3);
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_EINVAL);
    make_toc(b);
    sbm_put32(b + 32 * 2 + 12, 0x100);                               /* MAIN inside the TOC header */
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_ERANGE);
    memset(b, 0xFF, sizeof(b));                                      /* erased flash */
    CHECKEQ(sbm_toc_parse(b, 0, &t), SBM_EINVAL);

    /* plan refusals */
    struct sbm_plan p;
    make_toc(b);
    sbm_put32(b + 32 * 4 + 16, 0x47850000);                          /* NV past the end of cp_size */
    CHECKEQ(sbm_toc_parse(b, 0, &t), 0);
    CHECKEQ(sbm_plan_build(&t, &m, &p), SBM_ERANGE);
    make_toc(b);
    sbm_put32(b + 32 * 1 + 16, 0x50000000);                          /* BOOT not at the CP's base */
    CHECKEQ(sbm_toc_parse(b, 0, &t), 0);
    CHECKEQ(sbm_plan_build(&t, &m, &p), SBM_EINVAL);
    make_toc(b);
    sbm_put32(b + 32 * 4 + 16, 0x40011000);                          /* NV over MAIN */
    CHECKEQ(sbm_toc_parse(b, 0, &t), 0);
    CHECKEQ(sbm_plan_build(&t, &m, &p), SBM_ERANGE);

    /* memory map: broken tables fall back to the DT values */
    uint8_t mm[SBM_MEMMAP_BYTES];
    make_memmap(mm);
    sbm_put32(mm + 20 + 12, 0x08000004);                             /* IPC not contiguous with VSS */
    sbm_memmap_parse(mm, &m);
    CHECK(!m.valid);
    CHECKEQ(m.ipc_off, SBM_DT_IPC_OFF);
    make_memmap(mm);
    sbm_put32(mm + 12, 6);
    sbm_memmap_parse(mm, &m);
    CHECK(!m.valid);
    make_memmap(mm);
    mm[1] = 'X';
    sbm_memmap_parse(mm, &m);
    CHECK(!m.valid);
    make_memmap(mm);
    sbm_put32(mm + 24 + 12 * 4, 0x03000000);                         /* LOG past the carve-outs */
    sbm_memmap_parse(mm, &m);
    CHECK(!m.valid);
    sbm_memmap_parse(0, &m);
    CHECK(!m.valid && m.cp_size == SBM_DT_CP_SIZE);
}

/* The phone's own RADIO image, when given: read-only, first 1 KB + size. */
static int test_radio(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("radio: %s not readable, skipped\n", path);
        return 0;
    }
    uint8_t b[1024];
    size_t n = fread(b, 1, sizeof(b), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    CHECKEQ(n, sizeof(b));
    struct sbm_toc t;
    struct sbm_memmap m;
    CHECKEQ(sbm_toc_parse(b, (uint64_t)size, &t), 0);
    CHECK(t.boot >= 0 && t.main >= 0 && t.vss >= 0 && t.nv >= 0);
    sbm_memmap_parse(b + SBM_MEMMAP_OFF, &m);
    check_phone_map(&m);
    check_phone_plan(&t, &m);
    for (unsigned i = 0; i < t.n; ++i)
        printf("radio: %-6s off 0x%08x load 0x%08x size 0x%08x id %u\n", t.e[i].name, t.e[i].offset, t.e[i].loadaddr,
               t.e[i].size, t.e[i].entryid);
    printf("radio: memory map %s, IPC at +0x%08x\n", m.id, m.ipc_off);
    return 1;
}

/* ---- SIPC5 ---------------------------------------------------------------------------------- */

static void test_sipc5(void)
{
    /* logged: "LNK-RX(12): f8 eb 0c 00 08 00 01 00 01 01 03 53" */
    static const uint8_t pwrup[] = { 0xf8, 0xeb, 0x0c, 0x00, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x03, 0x53 };
    struct sbm_sipc5 f;
    struct sbm_ipc_msg m;
    CHECKEQ(sbm_sipc5_parse(pwrup, sizeof(pwrup), &f), 0);
    CHECKEQ(f.ch, 235);
    CHECKEQ(f.hdr_len, 4);
    CHECKEQ(f.frame_len, 12);
    CHECKEQ(f.payload_len, 8);
    CHECKEQ(sbm_ipc_parse(f.payload, f.payload_len, &m), 0);
    CHECKEQ(m.cmd, SBM_PWR_PHONE_PWR_UP);
    CHECKEQ(m.type, SBM_IPC_NOTI);
    CHECKEQ(m.mseq, 1);
    CHECKEQ(m.data_len, 1);
    CHECKEQ(m.data[0], 0x53);

    /* build round trip, and the request the kernel logged: "f8 eb 0d 00 09 00 42 00 08 05 02 ff 02" */
    uint8_t out[64];
    static const uint8_t want[] = { 0xf8, 0xeb, 0x0d, 0x00, 0x09, 0x00, 0x42, 0x00, 0x08, 0x05, 0x02, 0xff, 0x02 };
    uint8_t rq[2] = { 0xff, SBM_DOMAIN_CS };
    CHECKEQ(sbm_ipc_frame(out, sizeof(out), SBM_CH_FMT0, 0x42, SBM_NET_REGIST, SBM_IPC_GET, rq, 2), 13);
    CHECK(!memcmp(out, want, sizeof(want)));
    CHECKEQ(sbm_ipc_frame(out, 12, SBM_CH_FMT0, 0x42, SBM_NET_REGIST, SBM_IPC_GET, rq, 2), SBM_ENOSPC);
    CHECKEQ(sbm_ipc_frame(out, sizeof(out), SBM_CH_RFS0, 1, SBM_NET_REGIST, SBM_IPC_GET, rq, 2), SBM_EINVAL);

    /* extended length (> 64 KB) */
    static uint8_t big[70000 + 6], bigp[70000];
    for (unsigned i = 0; i < sizeof(bigp); ++i)
        bigp[i] = (uint8_t)i;
    CHECKEQ(sbm_sipc5_build(big, sizeof(big), 10, bigp, sizeof(bigp)), 70006);
    CHECKEQ(big[0], 0xFA);
    CHECKEQ(sbm_sipc5_parse(big, sizeof(big), &f), 0);
    CHECKEQ(f.hdr_len, 6);
    CHECKEQ(f.payload_len, 70000);
    CHECK(!memcmp(f.payload, bigp, 70000));
    CHECKEQ(sbm_sipc5_build(big, 1000, 10, bigp, sizeof(bigp)), SBM_ENOSPC);

    /* padding: two frames in one stream */
    uint8_t s[32] = { 0xFC, 235, 5, 0, 0xAA, 0, 0, 0, 0xFC, 236, 6, 0, 0xBB, 0xCC };
    CHECKEQ(sbm_sipc5_parse(s, sizeof(s), &f), 0);
    CHECKEQ(f.total, 8);
    CHECKEQ(sbm_sipc5_parse(s + f.total, 14 - 8, &f), 0);
    CHECKEQ(f.ch, 236);
    CHECKEQ(f.total, 6);                   /* last frame: padding cut at the end of the buffer */

    /* malformed */
    uint8_t bad[8] = { 0x78, 235, 8, 0 };
    CHECKEQ(sbm_sipc5_parse(bad, 8, &f), SBM_EBADF);
    bad[0] = 0xF8; bad[2] = 3;
    CHECKEQ(sbm_sipc5_parse(bad, 8, &f), SBM_EBADF);       /* length shorter than the header */
    bad[2] = 9;
    CHECKEQ(sbm_sipc5_parse(bad, 8, &f), SBM_ESHORT);      /* longer than the buffer */
    CHECKEQ(sbm_sipc5_parse(bad, 3, &f), SBM_ESHORT);
    bad[0] = 0xFB; bad[2] = 4;
    CHECKEQ(sbm_sipc5_parse(bad, 8, &f), SBM_EBADF);       /* multi: 5-byte header */
    bad[0] = 0xFA; sbm_put32(bad + 2, 0x10000000);
    CHECKEQ(sbm_sipc5_parse(bad, 8, &f), SBM_ESHORT);

    /* multi-frame reassembly */
    uint8_t msg[300], fr[64], rbuf[512];
    for (unsigned i = 0; i < sizeof(msg); ++i)
        msg[i] = (uint8_t)(i * 7);
    struct sbm_reasm r = { rbuf, sizeof(rbuf), 0, 0, 0, 0 };
    const uint8_t *out_m = 0;
    uint32_t out_l = 0;
    int ready = 0;
    for (unsigned o = 0; o < sizeof(msg); o += 50) {
        int l = sbm_sipc5_build_multi(fr, sizeof(fr), 236, 9, o + 50 < sizeof(msg), msg + o, 50);
        CHECKEQ(l, 55);
        CHECKEQ(sbm_sipc5_parse(fr, (uint32_t)l, &f), 0);
        CHECK(f.multi && f.id == 9);
        ready = sbm_reasm_feed(&r, &f, &out_m, &out_l);
        CHECKEQ(ready, o + 50 < sizeof(msg) ? 0 : 1);
    }
    CHECKEQ(out_l, sizeof(msg));
    CHECK(out_m && !memcmp(out_m, msg, sizeof(msg)));
    /* overflow drops the message */
    struct sbm_reasm small = { rbuf, 60, 0, 0, 0, 0 };
    sbm_sipc5_build_multi(fr, sizeof(fr), 236, 1, 1, msg, 50);
    sbm_sipc5_parse(fr, 55, &f);
    CHECKEQ(sbm_reasm_feed(&small, &f, &out_m, &out_l), 0);
    CHECKEQ(sbm_reasm_feed(&small, &f, &out_m, &out_l), SBM_ENOSPC);
    CHECKEQ(small.dropped, 1);
    /* a single frame in the middle drops the partial message and is delivered itself */
    sbm_reasm_feed(&small, &f, &out_m, &out_l);
    sbm_sipc5_parse(pwrup, sizeof(pwrup), &f);
    CHECKEQ(sbm_reasm_feed(&small, &f, &out_m, &out_l), 1);
    CHECKEQ(out_l, 8);
    CHECKEQ(small.dropped, 2);
}

/* ---- SBD ------------------------------------------------------------------------------------ */

static uint8_t ipc[SBM_DT_IPC_SIZE] __attribute__((aligned(16)));

static void test_sbd_layout(void)
{
    struct sbm_sbd s, a;
    CHECKEQ(sbm_sbd_init(&s, ipc, sizeof(ipc), sbm_starlte_chans, sbm_starlte_nchans), 0);
    /* golden: this phone's kernel log, 2026-11-08 091233 */
    CHECKEQ(SBM_SBD_DESC_OFF, 2048);
    CHECKEQ(SBM_SBD_RBCH_OFF, 2060);
    CHECKEQ(SBM_SBD_RBDESC_OFF, 2572);
    CHECKEQ(s.rbps_off, 3852);
    static const uint32_t sbdv[9][2] = {
        { 3924, 4052 }, { 4180, 4308 }, { 4564, 4692 }, { 4948, 5076 }, { 9172, 9428 },
        { 9940, 10068 }, { 10196, 10324 }, { 11348, 13396 }, { 15444, 19540 },
    };
    static const uint32_t buff[9][2] = {
        { 65536, 98304 }, { 131072, 196608 }, { 327680, 393216 }, { 524288, 557056 }, { 1605632, 1671168 },
        { 1802240, 1835008 }, { 1867776, 1900544 }, { 2162688, 2686976 }, { 3211264, 4259840 },
    };
    for (unsigned i = 0; i < 9; ++i)
        for (unsigned d = 0; d < 2; ++d) {
            CHECKEQ(s.r[i][d].rbd_off, 2572 + 20 * (2 * i + d));
            CHECKEQ(s.r[i][d].addr_v_off, sbdv[i][d]);
            CHECKEQ(s.r[i][d].buff_off, buff[i][d]);
        }
    CHECKEQ(s.buff_end, 4259840 + 16384);
    CHECKEQ(sbm_le32(ipc + 2048), 1);
    CHECKEQ(sbm_le32(ipc + 2052), 9);
    CHECKEQ(sbm_le32(ipc + 2056), 3852);
    /* RB_DESC[3][DL] = { ch 245, DL, signaling 1, mask 0x81, len 512, id 3, size 2048 } */
    const uint8_t *rd = ipc + 2712;
    CHECKEQ(sbm_le16(rd), 245);
    CHECKEQ(sbm_le16(rd + 4), 1);
    CHECKEQ(sbm_le16(rd + 6), 1);
    CHECKEQ(sbm_le32(rd + 8), 0x81);
    CHECKEQ(sbm_le16(rd + 12), 512);
    CHECKEQ(sbm_le16(rd + 14), 3);
    CHECKEQ(sbm_le16(rd + 16), 2048);
    CHECKEQ(sbm_le32(ipc + 2060 + 3 * 16 + 12), 5076);

    CHECKEQ(sbm_sbd_attach(&a, ipc, sizeof(ipc)), 0);
    CHECKEQ(a.n, 9);
    CHECK(!memcmp(a.r, s.r, sizeof(s.r)));
    CHECKEQ(sbm_sbd_id(&a, 236), 2);
    CHECKEQ(sbm_sbd_id(&a, 99), -1);

    /* too small a region, duplicate channel */
    CHECKEQ(sbm_sbd_init(&s, ipc, 0x100000, sbm_starlte_chans, sbm_starlte_nchans), SBM_ENOSPC);
    struct sbm_chan dup[2] = { sbm_starlte_chans[1], sbm_starlte_chans[1] };
    CHECKEQ(sbm_sbd_init(&s, ipc, sizeof(ipc), dup, 2), SBM_EINVAL);
}

static void test_sbd_rings(void)
{
    struct sbm_sbd ap, cp;
    const uint8_t *f;
    uint8_t fr[4096];
    CHECKEQ(sbm_sbd_init(&ap, ipc, sizeof(ipc), sbm_starlte_chans, sbm_starlte_nchans), 0);
    CHECKEQ(sbm_sbd_attach(&cp, ipc, sizeof(ipc)), 0);
    int id = sbm_sbd_id(&ap, SBM_CH_FMT0);
    /* UL fills at len - 1 */
    unsigned put = 0;
    for (;;) {
        memset(fr, (int)put, 100);
        int e = sbm_sbd_put(&ap, SBM_CH_FMT0, SBM_UL, fr, 100, 0);
        if (e == SBM_ENOSPC)
            break;
        CHECKEQ(e, 0);
        if (++put > 100)
            break;
    }
    CHECKEQ(put, 15);
    CHECKEQ(sbm_sbd_put(&ap, SBM_CH_FMT0, SBM_UL, fr, 4097, 0), SBM_ERANGE);
    CHECKEQ(sbm_sbd_put(&ap, 77, SBM_UL, fr, 10, 0), SBM_EINVAL);
    /* the CP drains them in order; then wrap-around */
    for (unsigned k = 0; k < 40; ++k) {
        if (k >= 15) {
            memset(fr, (int)k, 100);
            CHECKEQ(sbm_sbd_put(&ap, SBM_CH_FMT0, SBM_UL, fr, 100, 0), 0);
        }
        int n = sbm_sbd_get(&cp, (unsigned)id, SBM_UL, &f, 0);
        CHECKEQ(n, 100);
        CHECK(f && f[0] == (uint8_t)k && f[99] == (uint8_t)k);
        sbm_sbd_get_done(&cp, (unsigned)id, SBM_UL, 0);
    }
    CHECKEQ(sbm_sbd_get(&cp, (unsigned)id, SBM_UL, &f, 0), 0);
    CHECKEQ(sbm_sbd_rp(&ap, (unsigned)id, SBM_UL), 40 % 16);

    /* PS channel: size_v carries ch << 16 */
    int pid = sbm_sbd_id(&ap, SBM_CH_PDP0);
    CHECKEQ(sbm_sbd_put(&cp, SBM_CH_PDP0, SBM_DL, fr, 1500, 0), 0);
    CHECKEQ(sbm_le32(ipc + ap.r[pid][SBM_DL].size_v_off), (SBM_CH_PDP0 << 16) | 1500);
    CHECKEQ(sbm_sbd_get(&ap, (unsigned)pid, SBM_DL, &f, 0), 1500);
    sbm_sbd_get_done(&ap, (unsigned)pid, SBM_DL, 0);

    /* a CP that writes nonsense: an index past the ring, a size past the buffer */
    int did = sbm_sbd_id(&ap, SBM_CH_FMT1);
    uint32_t wp_off = ap.rbps_off + 2 * (ap.n * 3 + (unsigned)did);
    uint16_t save = sbm_le16(ipc + wp_off);
    sbm_put16(ipc + wp_off, 999);
    CHECKEQ(sbm_sbd_get(&ap, (unsigned)did, SBM_DL, &f, 0), SBM_EIO);
    CHECK(f == 0);
    sbm_put16(ipc + wp_off, save);
    CHECKEQ(sbm_sbd_put(&cp, SBM_CH_FMT1, SBM_DL, fr, 64, 0), 0);
    uint16_t rp = sbm_sbd_rp(&ap, (unsigned)did, SBM_DL);
    sbm_put32(ipc + ap.r[did][SBM_DL].size_v_off + 4 * rp, 5000);
    CHECKEQ(sbm_sbd_get(&ap, (unsigned)did, SBM_DL, &f, 0), SBM_EBADF);   /* skipped */
    CHECKEQ(sbm_sbd_get(&ap, (unsigned)did, SBM_DL, &f, 0), 0);
    CHECK(ap.errors >= 2);

    /* attach refuses a corrupted descriptor region */
    struct sbm_sbd x;
    uint32_t keep = sbm_le32(ipc + 2060 + 16 * 3 + 4);
    sbm_put32(ipc + 2060 + 16 * 3 + 4, 0x4fff0);                  /* SBD vector outside the descriptor region */
    CHECKEQ(sbm_sbd_attach(&x, ipc, sizeof(ipc)), SBM_EBADF);
    sbm_put32(ipc + 2060 + 16 * 3 + 4, keep);
    keep = sbm_le32(ipc + 5076);
    sbm_put32(ipc + 5076, 0x4ff000);                               /* buffers past the region */
    CHECKEQ(sbm_sbd_attach(&x, ipc, sizeof(ipc)), SBM_EBADF);
    sbm_put32(ipc + 5076, keep);
    CHECKEQ(sbm_sbd_attach(&x, ipc, sizeof(ipc)), 0);

    /* fuzz: random flips in the descriptor region; attach either refuses or every ring stays inside */
    srand(1);
    static uint8_t save_desc[SBM_SBD_DESC_END];
    memcpy(save_desc, ipc, sizeof(save_desc));
    unsigned ok = 0;
    for (unsigned it = 0; it < 3000; ++it) {
        memcpy(ipc, save_desc, sizeof(save_desc));
        for (int k = 0; k < 1 + rand() % 4; ++k)
            ipc[SBM_SBD_DESC_OFF + (unsigned)rand() % (20000 - SBM_SBD_DESC_OFF)] ^= (uint8_t)(1u << (rand() % 8));
        if (sbm_sbd_attach(&x, ipc, sizeof(ipc)) != 0)
            continue;
        ok++;
        for (unsigned i = 0; i < x.n; ++i)
            for (unsigned d = 0; d < 2; ++d) {
                const struct sbm_ring *r = &x.r[i][d];
                CHECK((uint64_t)r->buff_off + (uint64_t)r->len * r->size <= sizeof(ipc));
                CHECK(r->size_v_off + 4u * r->len <= SBM_SBD_DESC_END);
                /* exercise it (ASan watches) */
                int n = sbm_sbd_get(&x, i, d, &f, 0);
                if (n > 0)
                    CHECK(f >= ipc && f + n <= ipc + sizeof(ipc));
                sbm_sbd_put(&x, r->ch, d, fr, r->size < 16 ? r->size : 16, 0);
            }
    }
    printf("sbd fuzz: %u of 3000 corrupted layouts attached, all inside the region\n", ok);
    memcpy(ipc, save_desc, sizeof(save_desc));
}

/* ---- IPC codec and the allow-list ------------------------------------------------------------ */

static void test_ipc(void)
{
    uint8_t out[64];
    unsigned allowed = 0;
    for (unsigned cmd = 0; cmd < 0x10000; ++cmd)
        for (unsigned type = 0; type < 256; ++type) {
            if (!sbm_ipc_allowed((uint16_t)cmd, (uint8_t)type))
                continue;
            allowed++;
            CHECK(type == SBM_IPC_GET);
            CHECK((cmd >> 8) != SBM_G_CALL && (cmd >> 8) != SBM_G_SMS && (cmd >> 8) != SBM_G_SS &&
                  (cmd >> 8) != SBM_G_SAT && (cmd >> 8) != SBM_G_IMEI && (cmd >> 8) != SBM_G_RFS);
        }
    CHECKEQ(allowed, 11);
    /* every type of every CALL / SMS command is refused by the builder */
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned type = 0; type < 8; ++type) {
            CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_CMD(SBM_G_CALL, i), (uint8_t)type, 0, 0), SBM_EPERM);
            CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_CMD(SBM_G_SMS, i), (uint8_t)type, 0, 0), SBM_EPERM);
            CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_CMD(SBM_G_SS, i), (uint8_t)type, 0, 0), SBM_EPERM);
        }
    /* identities and anything that changes state */
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_MISC_ME_IMSI, SBM_IPC_GET, 0, 0), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_MISC_ME_SN, SBM_IPC_GET, 0, 0), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_NET_SUBSCRIBER_NUM, SBM_IPC_GET, 0, 0), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_SEC_RSIM_ACCESS, SBM_IPC_GET, 0, 0), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_NET_PLMN_SEL, SBM_IPC_SET, 0, 0), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_CMD(SBM_G_PWR, 0x02), SBM_IPC_EXEC, 0, 0), SBM_EPERM);
    uint8_t three[3] = { 1, 2, 3 };
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 1, SBM_NET_REGIST, SBM_IPC_GET, three, 3), SBM_EPERM);
    CHECKEQ(sbm_ipc_build(out, sizeof(out), 7, SBM_SEC_PIN_STATUS, SBM_IPC_GET, 0, 0), 7);
    CHECK(out[0] == 7 && out[1] == 0 && out[2] == 7 && out[3] == 0 && out[4] == 5 && out[5] == 1 && out[6] == 2);

    /* parse refusals */
    struct sbm_ipc_msg m;
    uint8_t b[8] = { 6, 0, 1, 0, 1, 1, 3 };
    CHECKEQ(sbm_ipc_parse(b, 7, &m), SBM_EBADF);
    sbm_put16(b, 9);
    CHECKEQ(sbm_ipc_parse(b, 8, &m), SBM_EBADF);
    CHECKEQ(sbm_ipc_parse(b, 6, &m), SBM_ESHORT);

    /* GEN_PHONE_RES as logged: "80 01 02 0f 3b 02 01 80" (ok) and "... 05 0a 02 0a 00" (error 0x0a) */
    static const uint8_t ok[] = { 12, 0, 3, 3, 0x80, 0x01, 0x02, 0x0f, 0x3b, 0x02, 0x01, 0x80 };
    static const uint8_t er[] = { 12, 0, 0x0f, 8, 0x80, 0x01, 0x02, 0x05, 0x0a, 0x02, 0x0a, 0x00 };
    struct sbm_gen_res r;
    CHECKEQ(sbm_ipc_parse(ok, sizeof(ok), &m), 0);
    CHECKEQ(sbm_gen_res_decode(&m, &r), 0);
    CHECKEQ(r.cmd, 0x0f3b);
    CHECK(sbm_gen_res_ok(&r));
    CHECKEQ(sbm_ipc_parse(er, sizeof(er), &m), 0);
    CHECKEQ(sbm_gen_res_decode(&m, &r), 0);
    CHECKEQ(r.cmd, 0x050a);
    CHECKEQ(r.code, 0x000a);
    CHECK(!sbm_gen_res_ok(&r));

    char nb[12];
    CHECK(!strcmp(sbm_ipc_name(SBM_NET_REGIST, nb), "NET_REGIST"));
    CHECK(!strcmp(sbm_ipc_name(0x2601, nb), "0x2601"));
    CHECK(!strcmp(sbm_ipc_type_name(SBM_IPC_NOTI, 1), "NOTI"));
    CHECK(!strcmp(sbm_ipc_type_name(SBM_IPC_GET, 0), "GET"));
}

/* ---- the AP side of the test: drain the FMT rings into the cell state ---------------------- */

struct ap {
    struct sbm_sbd sbd;
    struct sbm_cell_state st;
    struct sbm_reasm ra[2];
    uint8_t rbuf[2][8192];
    uint8_t mseq;
    uint32_t n_rx, n_bad;
    uint16_t last_cmd;
    uint8_t last_aseq;
};

static int ap_poll(struct ap *a)
{
    static const uint8_t fmt[2] = { SBM_CH_FMT0, SBM_CH_FMT1 };
    int n_changed = 0;
    for (unsigned k = 0; k < 2; ++k) {
        int id = sbm_sbd_id(&a->sbd, fmt[k]);
        const uint8_t *f;
        int n;
        while ((n = sbm_sbd_get(&a->sbd, (unsigned)id, SBM_DL, &f, 0)) != 0) {
            if (n < 0) {
                a->n_bad++;
                continue;
            }
            struct sbm_sipc5 fr;
            const uint8_t *msg;
            uint32_t ml;
            struct sbm_ipc_msg m;
            if (sbm_sipc5_parse(f, (uint32_t)n, &fr) == 0 && fr.ch == fmt[k]) {
                int r = sbm_reasm_feed(&a->ra[k], &fr, &msg, &ml);
                if (r == 1 && sbm_ipc_parse(msg, ml, &m) == 0) {
                    a->n_rx++;
                    a->last_cmd = m.cmd;
                    a->last_aseq = m.aseq;
                    n_changed += sbm_cell_update(&a->st, &m);
                } else if (r < 0 || (r == 1)) {
                    a->n_bad++;
                }
            } else {
                a->n_bad++;
            }
            sbm_sbd_get_done(&a->sbd, (unsigned)id, SBM_DL, 0);
        }
    }
    return n_changed;
}

static int ap_get(struct ap *a, uint16_t cmd, const uint8_t *d, uint16_t n)
{
    uint8_t f[64];
    int l = sbm_ipc_frame(f, sizeof(f), SBM_CH_FMT0, ++a->mseq, cmd, SBM_IPC_GET, d, n);
    if (l < 0)
        return l;
    return sbm_sbd_put(&a->sbd, SBM_CH_FMT0, SBM_UL, f, (uint32_t)l, 0);
}

static void ap_setup(struct ap *a)
{
    memset(a, 0, sizeof(*a));
    sbm_cell_init(&a->st);
    for (int k = 0; k < 2; ++k) {
        a->ra[k].buf = a->rbuf[k];
        a->ra[k].cap = sizeof(a->rbuf[k]);
    }
}

static void test_e2e(void)
{
    static struct ap a;
    struct fakecp cp;
    char line[160];

    /* no SIM: the state this phone is in */
    ap_setup(&a);
    memset(ipc, 0, sizeof(ipc));
    CHECKEQ(sbm_sbd_init(&a.sbd, ipc, sizeof(ipc), sbm_starlte_chans, sbm_starlte_nchans), 0);
    CHECKEQ(fakecp_attach(&cp, ipc, sizeof(ipc), FAKE_NOSIM), 0);
    fakecp_boot_notis(&cp);
    CHECK(ap_poll(&a) > 0);
    CHECKEQ(a.n_rx, 6);
    sbm_cell_line(&a.st, line, sizeof(line));
    printf("e2e no SIM: %s\n", line);
    CHECK(!strcmp(line, "SIM absent | CS emergency-only (GSM) | PS emergency-only (GSM) | PLMN - | bars -"));
    CHECKEQ(a.st.pwr_up, 0x53);
    CHECKEQ(a.st.phone_state, 2);
    CHECK(a.st.cs.have_loc);

    /* every allowed GET, answered with aseq = our mseq */
    uint8_t rq[2] = { 0xff, SBM_DOMAIN_PS }, icon = 1;
    CHECKEQ(ap_get(&a, SBM_SEC_PIN_STATUS, 0, 0), 0);
    CHECKEQ(fakecp_step(&cp), 1);
    ap_poll(&a);
    CHECKEQ(a.last_cmd, SBM_SEC_PIN_STATUS);
    CHECKEQ(a.last_aseq, a.mseq);
    CHECKEQ(ap_get(&a, SBM_NET_REGIST, rq, 2), 0);
    CHECKEQ(ap_get(&a, SBM_NET_SERVING_NETWORK, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_NET_PLMN_SEL, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_DISP_ICON_INFO, &icon, 1), 0);
    CHECKEQ(ap_get(&a, SBM_DISP_RSSI_INFO, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_PWR_PHONE_STATE, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_SEC_SIM_ICC_TYPE, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_MISC_TIME_INFO, 0, 0), 0);      /* the fake CP refuses it: GEN_PHONE_RES 0x0a */
    CHECKEQ(fakecp_step(&cp), 8);
    ap_poll(&a);
    CHECKEQ(a.st.n_gen_err, 1);
    CHECKEQ(a.st.plmn_sel, 2);
    CHECK(a.st.have_bars && a.st.bars == 0);
    CHECK(a.st.have_rssi && a.st.rssi_raw[0] == 0x3e);
    CHECK(!a.st.have_plmn);
    CHECKEQ(ap_get(&a, SBM_MISC_ME_IMSI, 0, 0), SBM_EPERM);  /* never reaches the ring */

    /* the SIM appears and the network registers */
    fakecp_set_scenario(&cp, FAKE_HOME);
    CHECK(ap_poll(&a) > 0);
    CHECKEQ(ap_get(&a, SBM_NET_SERVING_NETWORK, 0, 0), 0);
    CHECKEQ(ap_get(&a, SBM_DISP_ICON_INFO, &icon, 1), 0);
    fakecp_step(&cp);
    ap_poll(&a);
    sbm_cell_line(&a.st, line, sizeof(line));
    printf("e2e home:   %s\n", line);
    CHECK(!strcmp(line, "SIM ready (init complete) | CS home (LTE) | PS home (LTE) | PLMN 00101 | bars 3"));

    /* a multi-frame message from the CP */
    uint8_t big[3000];
    memset(big, 0, sizeof(big));
    big[0] = 0x02; big[1] = 0x01; big[2] = 0xff;
    memcpy(big + 3, "310410", 6);
    CHECKEQ(fakecp_send_multi(&cp, SBM_CH_FMT1, 5, SBM_NET_SERVING_NETWORK, SBM_IPC_NOTI, big, sizeof(big), 1000), 0);
    ap_poll(&a);
    CHECK(!strcmp(a.st.plmn, "310410"));

    /* garbage on the ring: counted, never fatal */
    static const uint8_t junk1[] = { 0x00, 0x01, 0x02, 0x03, 0x04 };
    static const uint8_t junk2[] = { 0xf8, 0xeb, 0x09, 0x00, 0x30, 0x00, 0x01, 0x00, 0x08 };   /* IPC length lies */
    static const uint8_t junk3[] = { 0xf8, 0xec, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04 };         /* frame on the wrong ring */
    fakecp_raw(&cp, SBM_CH_FMT0, junk1, sizeof(junk1));
    fakecp_raw(&cp, SBM_CH_FMT0, junk2, sizeof(junk2));
    fakecp_raw(&cp, SBM_CH_FMT0, junk3, sizeof(junk3));
    uint32_t before = a.n_bad;
    ap_poll(&a);
    CHECKEQ(a.n_bad - before, 3);

    /* the CP never saw a call, an SMS or anything off the allow-list */
    CHECKEQ(cp.n_call, 0);
    CHECKEQ(cp.n_forbidden, 0);
    CHECKEQ(cp.n_bad, 0);
}

/* ---- decoders, redaction ------------------------------------------------------------------- */

static void test_cell_and_redact(void)
{
    struct sbm_cell_state st;
    struct sbm_ipc_msg m;
    sbm_cell_init(&st);
    /* logged NET_REGIST NOTI head: "01 02 04 00 37 .." (CS, emergency-only) */
    uint8_t reg[7 + 19] = { 26, 0, 0x3e, 0, 0x08, 0x05, 0x03, 0x01, 0x02, 0x04, 0x00, 0x37, 0x11 };
    CHECKEQ(sbm_ipc_parse(reg, sizeof(reg), &m), 0);
    CHECKEQ(sbm_cell_update(&st, &m), 1);
    CHECK(st.have_cs && st.cs.state == SBM_REG_EMERGENCY && st.cs.act == SBM_ACT_GSM && st.cs.have_loc);
    CHECKEQ(sbm_cell_update(&st, &m), 0);        /* same again: no change */
    reg[8] = 0x09;                               /* unknown domain */
    sbm_ipc_parse(reg, sizeof(reg), &m);
    CHECKEQ(sbm_cell_update(&st, &m), 0);
    CHECKEQ(st.n_bad, 1);
    uint8_t shortpin[8] = { 8, 0, 1, 0, 0x05, 0x01, 0x03, 0x80 };
    sbm_ipc_parse(shortpin, sizeof(shortpin), &m);
    CHECKEQ(sbm_cell_update(&st, &m), 0);
    CHECKEQ(st.n_bad, 2);
    CHECK(!strcmp(sbm_sim_name(SBM_SIM_ABSENT), "absent"));

    char s[200];
    strcpy(s, "IMEI 012345678901234 IMSI 001010123456789 cid 7654321 len 123456");
    CHECKEQ(sbm_redact_text(s, sizeof(s)), 3);
    CHECK(!strcmp(s, "IMEI ############### IMSI ############### cid ####### len 123456"));
    strcpy(s, "magic 0x424f4f54 at 0xf0000000 val 0x00000005 big 0x0000000000000001");
    CHECKEQ(sbm_redact_text(s, sizeof(s)), 0);
    strcpy(s, "ef 0809101032547698f0 iccid 89000000000000000001");
    CHECKEQ(sbm_redact_text(s, sizeof(s)), 2);
    CHECK(strstr(s, "0809") == 0 && strstr(s, "8901") == 0);
    strcpy(s, "a0x1234567");                /* not a literal: the digits are masked */
    CHECKEQ(sbm_redact_text(s, sizeof(s)), 1);

    uint8_t ef[9] = { 8, 0x09, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98 };    /* 001010123456789 */
    char mm[8];
    CHECKEQ(sbm_imsi_mccmnc(ef, 2, mm), 15);
    CHECK(!strcmp(mm, "00101"));
    CHECKEQ(sbm_imsi_mccmnc(ef, 3, mm), 15);
    CHECK(!strcmp(mm, "001010"));
    ef[0] = 1;
    CHECKEQ(sbm_imsi_mccmnc(ef, 2, mm), SBM_EBADF);

    /* the frame dumper never shows an identity payload */
    uint8_t f[64];
    uint8_t sn[] = "012345678901234";
    int l = 4 + 7 + 15;
    f[0] = 0xf8; f[1] = 0xeb; sbm_put16(f + 2, (uint16_t)l);
    sbm_put16(f + 4, 7 + 15); f[6] = 1; f[7] = 2; f[8] = 0x0a; f[9] = 0x03; f[10] = 0x02;
    memcpy(f + 11, sn, 15);
    sbm_frame_line(f, (uint32_t)l, 1, s, sizeof(s));
    CHECK(strstr(s, "MISC_ME_SN RESP") && strstr(s, "<15 bytes>") && !strstr(s, "30 31"));
    static const uint8_t pin[] = { 0xf8, 0xec, 0x0d, 0x00, 0x09, 0x00, 0x0e, 0x00, 0x05, 0x01, 0x03, 0x80, 0x00 };
    sbm_frame_line(pin, sizeof(pin), 1, s, sizeof(s));
    CHECK(!strcmp(s, "RX ch236 len 13 | SEC_PIN_STATUS NOTI seq 14/0 | 80 00"));
    sbm_frame_line(pin, 5, 1, s, sizeof(s));
    CHECK(strstr(s, "bad frame") != 0);

    /* sbm_logf redacts what a caller formats */
    struct sbm_plat p = { 0, 0, tlog, 0 };
    sbm_logf(&p, "number %s reg 0x%08x", "+15555550100", 0x6ba2u);
    CHECK(!strcmp(lastlog, "number +########### reg 0x00006ba2"));
    sbm_logf(&p, "%d %u %x %5s|%3d|%c%%|%04x|%llu", -42, 7u, 0xabcu, "ab", 5, 'z', 0x1fu, 123456ULL);
    CHECK(!strcmp(lastlog, "-42 7 abc    ab|  5|z%|001f|123456"));
}

/* ---- stage 2 probe against the register model --------------------------------------------- */

static int addr_allowed(uint64_t a)
{
    static const uint32_t pmu[] = { SBM_PMU_RESET_SEQ_STATUS, SBM_PMU_CP_STAT, SBM_PMU_CP_DEBUG,
                                    SBM_PMU_CENTRAL_SEQ_CP_CFG, SBM_PMU_CENTRAL_SEQ_CP_ST };
    for (unsigned i = 0; i < 5; ++i)
        if (a == SBM_PMU_PA + pmu[i])
            return 1;
    if (a >= SBM_MBOX_PA && a < SBM_MBOX_PA + SBM_MBOX_SIZE)
        return 1;
    uint64_t c = SBM_MODEM_IF_PA;
    if (a >= c && a < c + 16)
        return 1;
    if (a >= c + SBM_MEMMAP_OFF && a < c + SBM_MEMMAP_OFF + SBM_MEMMAP_BYTES)
        return 1;
    if (a >= c + SBM_DT_IPC_OFF && a < c + SBM_DT_IPC_OFF + 8)
        return 1;
    return 0;
}

static void check_trace(const struct fakeregs *r)
{
    for (unsigned i = 0; i < r->ntrace; ++i) {
        CHECK(addr_allowed(r->trace[i]));
        CHECK(r->trace[i] != SBM_PMU_PA + 0x30 && r->trace[i] != SBM_PMU_PA + 0x34);
    }
}

/* records, at the first read of each block, how many log lines were written before it */
static struct { struct fakeregs *inner; unsigned n; unsigned nlog_at[8]; uint64_t last_block; } crumbs;
static int crumb_probe32(void *ctx, uint64_t pa, uint32_t *v)
{
    (void)ctx;
    uint64_t c = SBM_MODEM_IF_PA, block = 0;
    if (pa >= SBM_PMU_PA && pa < SBM_PMU_PA + 0x10000)
        block = 1;
    else if (pa >= SBM_MBOX_PA && pa < SBM_MBOX_PA + SBM_MBOX_SIZE)
        block = 2;
    else if (pa >= c && pa < c + 16)
        block = 3;
    else if (pa >= c + SBM_MEMMAP_OFF && pa < c + SBM_MEMMAP_OFF + SBM_MEMMAP_BYTES)
        block = 4;
    else if (pa >= c + SBM_DT_IPC_OFF && pa < c + SBM_DT_IPC_OFF + 8)
        block = 5;
    if (block != crumbs.last_block && crumbs.n < 8)
        crumbs.nlog_at[crumbs.n++] = nlog;
    crumbs.last_block = block;
    return fakeregs_probe32(crumbs.inner, pa, v);
}

static void test_probe(void)
{
    static struct fakeregs r;
    struct sbm_plat p = { &r, fakeregs_probe32, tlog, 0 };
    struct sbm_probe pr;
    static uint8_t head[0x400];

    /* CP held in reset, nothing in the carve-out readable */
    memset(&r, 0, sizeof(r));
    r.pmu[SBM_PMU_CENTRAL_SEQ_CP_ST / 4] = 0x80u << 16;
    sbm_probe_run(&p, SBM_PROBE_REGS | SBM_PROBE_SHM, &pr);
    CHECK(pr.pmu_ok && pr.mbox_ok);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-in-reset"));
    CHECK(pr.shm_tried && !pr.shm_head_ok);
    CHECKEQ(r.shm_reads, 1);                       /* stopped at the first refused read */
    CHECKEQ(pr.faults, 1);
    check_trace(&r);
    nlog = 0;
    sbm_probe_report(&p, &pr);
    CHECK(nlog >= 5 && strstr(lastlog, "not AP-readable"));

    /* the mailbox values this phone logged just before ONLINE; registers only */
    memset(&r, 0, sizeof(r));
    r.pmu[SBM_PMU_RESET_SEQ_STATUS / 4] = 5;
    r.mbox[SBM_MBOX_ISSR(0) / 4] = 0xcd;
    r.mbox[SBM_MBOX_ISSR(1) / 4] = 0xc8;
    r.mbox[SBM_MBOX_ISSR(2) / 4] = 0x6ba2;
    r.mbox[SBM_MBOX_ISSR(3) / 4] = 0x22;
    sbm_probe_run(&p, SBM_PROBE_REGS, &pr);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-running"));
    CHECKEQ(SBM_AP_SYS_REV(pr.issr[2]), 174);
    CHECKEQ(SBM_AP_DS_DET(pr.issr[2]), 1);
    CHECKEQ(SBM_AP_PDA_ACTIVE(pr.issr[2]), 1);
    CHECKEQ(SBM_CP_STATUS(pr.issr[3]), 1);
    CHECKEQ(pr.issr_ok, 0xFFFF);
    CHECKEQ(r.shm_reads, 0);
    check_trace(&r);
    sbm_probe_report(&p, &pr);
    CHECK(strstr(lastlog, "not tried"));

    /* carve-out readable (S-Boot's copy of RADIO at + 0, the map at + 0x2a0) with IPC up */
    memset(&r, 0, sizeof(r));
    r.pmu[SBM_PMU_RESET_SEQ_STATUS / 4] = 5;
    make_toc(head);
    make_memmap(head + SBM_MEMMAP_OFF);
    r.shm_head = head;
    sbm_put32(r.ipc_head, SBM_IPC_MAGIC);
    sbm_put32(r.ipc_head + 4, 1);
    sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
    CHECK(pr.shm_head_ok && pr.shm_map_ok && pr.shm_ipc_ok);
    check_phone_map(&pr.mm);
    CHECK(!memcmp(pr.head, "TOC", 4));
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-online"));
    CHECKEQ(pr.faults, 0);
    check_trace(&r);
    sbm_put32(r.ipc_head, SBM_CRASH_MAGIC);
    sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-crashed"));
    sbm_put32(r.ipc_head, SBM_BOOT_MAGIC);
    sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-booting"));
    sbm_probe_report(&p, &pr);
    CHECK(strstr(lastlog, "magic 0x424f4f54 (BOOT)"));

    /* mailbox refuses (clock off): PMU still read, verdict from it */
    memset(&r, 0, sizeof(r));
    r.mbox_faults = 1;
    sbm_probe_run(&p, SBM_PROBE_REGS, &pr);
    CHECK(pr.pmu_ok && !pr.mbox_ok);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "cp-off"));
    CHECKEQ(pr.faults, 1);                         /* one try, then no more mailbox reads */
    check_trace(&r);

    /* breadcrumbs: before each block's first read, the log names it; the PMU values are logged before the
     * mailbox is touched (a hang then shows in pmsg where it was) */
    memset(&r, 0, sizeof(r));
    r.pmu[SBM_PMU_RESET_SEQ_STATUS / 4] = 5;
    r.shm_head = head;
    struct sbm_plat pc = { &crumbs, crumb_probe32, tlog, 0 };
    crumbs.inner = &r;
    crumbs.n = 0;
    nlog = 0;
    sbm_probe_run(&pc, SBM_PROBE_SHM, &pr);
    CHECKEQ(crumbs.n, 5);
    static const char *const want_crumb[5] = { "probe: reading PMU", "probe: reading mailbox",
                                               "probe: reading modem_if+0 ", "probe: reading modem_if+0x2a0",
                                               "probe: reading IPC region" };
    for (unsigned i = 0; i < 5 && i < crumbs.n; ++i) {
        unsigned at = crumbs.nlog_at[i];
        CHECK(at > 0 && !strncmp(loglines[(at - 1) % 256], want_crumb[i], strlen(want_crumb[i])));
    }
    CHECK(crumbs.n >= 2 && crumbs.nlog_at[1] >= 2 && !strncmp(loglines[crumbs.nlog_at[1] - 2], "PMU: RESET", 10));
    CHECK(!strcmp(loglines[(nlog - 1) % 256], "probe: carve-out reads done"));

    /* an all-zero carve-out with no fault: empty OR TZASC read-as-zero; no answer either way */
    static uint8_t zero[0x400];
    memset(&r, 0, sizeof(r));
    r.pmu[SBM_PMU_RESET_SEQ_STATUS / 4] = 5;
    r.shm_head = zero;
    sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
    CHECK(pr.shm_head_ok && pr.shm_map_ok && pr.shm_zero);
    CHECKEQ(pr.faults, 0);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "shm-zero-ambiguous"));
    nlog = 0;
    sbm_probe_report(&p, &pr);
    unsigned seen = 0;
    for (unsigned i = 0; i < nlog && i < 256; ++i)
        seen += !strcmp(loglines[i], "SHM: all zero: nothing loaded OR TZASC read-as-zero; not proof of access");
    CHECKEQ(seen, 1);
    CHECK(strstr(loglines[0], "verdict shm-zero-ambiguous") != 0);
    /* a valid map with a zero IPC word is an answer (readable), not ambiguous */
    r.shm_head = head;
    memset(r.ipc_head, 0, sizeof(r.ipc_head));
    sbm_probe_run(&p, SBM_PROBE_SHM, &pr);
    CHECK(!pr.shm_zero && pr.mm.valid);
    CHECK(strcmp(sbm_probe_verdict(&pr), "shm-zero-ambiguous") != 0);

    /* no probe32: reads nothing */
    struct sbm_plat none = { 0, 0, tlog, 0 };
    sbm_probe_run(&none, SBM_PROBE_SHM, &pr);
    CHECK(!strcmp(sbm_probe_verdict(&pr), "unreadable"));
}

int main(int argc, char **argv)
{
    test_toc();
    int radio = argc > 1 ? test_radio(argv[1]) : 0;
    test_sipc5();
    test_sbd_layout();
    test_sbd_rings();
    test_ipc();
    test_e2e();
    test_cell_and_redact();
    test_probe();
    printf("sbm host test: %u checks, %u failed%s\n", checks, fails, radio ? " (with this phone's RADIO image)" : "");
    return fails ? 1 : 0;
}
