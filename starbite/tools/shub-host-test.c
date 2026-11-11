/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the sensor hub's wire formats (starbite/sbos/kern/shub_proto.c; docs/SENSOR-HUB.md), run by
 * starbite/sbos/build.sh on a native host and by hand:
 *   gcc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Istarbite/sbos/kern -o /tmp/shub-host-test \
 *       starbite/tools/shub-host-test.c starbite/sbos/kern/shub_proto.c -lm && /tmp/shub-host-test [PATCH.bin]
 * Against the three sample frames of Samsung's GPL driver (bbdpl/bcm_gps_regs.c:71 and :76, bbd_rpc_lh.c's sample1)
 * and, with a patch blob (starbite/sbos/tools/shub-patch.py), the blob's 452 records. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shub_proto.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                             \
    do {                                                                                                         \
        ++checks;                                                                                                \
        if (!(c)) {                                                                                              \
            ++fails;                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

/* bcm_gps_regs.c:71 transaction_2nd without its leading SPI byte: the host's first sync request */
static const uint8_t gpl_req[] = { 0xB0, 0x00, 0x00, 0x02, 0x80, 0x03, 0x00, 0x00, 0x00, 0x01, 0xB1, 0xB0, 0x01 };
/* bcm_gps_regs.c:76 transaction_3rd without its SSI length: the chip's sync response */
static const uint8_t gpl_rsp[] = { 0xB0, 0x00, 0x00, 0x08, 0x80, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x43, 0xB0, 0x01 };
/* bbd_rpc_lh.c sample1 without its 2-byte SSI length: a sensor packet (older firmware: its record layout differs) */
static const uint8_t gpl_sample1[] = { 0xB0, 0x00, 0x85, 0x26, 0x00, 0x21, 0x24, 0x22, 0x00, 0x02, 0x00, 0x1E, 0x00,
    0x37, 0x0E, 0xD3, 0x00, 0xCB, 0xFF, 0x50, 0x00, 0xDE, 0x00, 0x56, 0x00, 0xC7, 0xFF, 0x02, 0x00,
    0x00, 0x00, 0x37, 0x00, 0x90, 0xFE, 0x18, 0xFF, 0xB7, 0x46, 0x02, 0x00, 0x00, 0x00, 0xB2, 0xB0, 0x01 };

static int feed(struct shp_rx *r, const uint8_t *p, unsigned n, struct shp_pkt *out)
{
    int got = 0;
    for (unsigned i = 0; i < n; ++i) {
        struct shp_pkt k;
        int v = shp_rx_byte(r, p[i], &k);
        if (v > 0) {
            *out = k;
            ++got;
        } else if (v < 0) {
            return -1;
        }
    }
    return got;
}

static void t_packets(void)
{
    static const uint8_t a[] = { 0x02, 0x80, 0x03, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t b[] = { 0x08, 0x80, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF };
    CHECK(shp_crc(a, sizeof(a)) == 0xB1, "crc of the GPL request (%02x)", shp_crc(a, sizeof(a)));
    CHECK(shp_crc(b, sizeof(b)) == 0x43, "crc of the GPL response (%02x)", shp_crc(b, sizeof(b)));

    uint8_t out[64];
    unsigned n = shp_sync_build(out, sizeof(out), 1);
    CHECK(n == sizeof(gpl_req) && !memcmp(out, gpl_req, n), "the first sync request is the GPL sample byte for byte (%u)", n);
    n = shp_sync_build(out, sizeof(out), 0x11);         /* an attempt number that needs stuffing */
    CHECK(n == sizeof(gpl_req) + 1 && out[9] == 0xB0 && out[10] == 0x04, "attempt 0x11 is stuffed");
    CHECK(shp_sync_build(out, 5, 1) == 0, "a buffer too small is refused");

    struct shp_rx r;
    struct shp_pkt p;
    struct shp_sync s;
    shp_rx_init(&r);
    CHECK(feed(&r, gpl_rsp, sizeof(gpl_rsp), &p) == 1, "the GPL response parses");
    CHECK(p.seq == 0 && p.len == 8 && (p.flags & SHP_F_INTERNAL) && !(p.flags & SHP_F_IGNORE_SEQ), "its header");
    CHECK(shp_sync_parse(&p, &s) == 1 && s.n == 1 && s.last_seq_rx == 0 && s.last_rel_rx == 0xFF && s.next_seq == 0 &&
          s.next_rel == 0 && s.last_rel_acked == 0xFF, "its sync fields");
    CHECK(feed(&r, gpl_req, sizeof(gpl_req), &p) == 1 && (p.flags & SHP_F_IGNORE_SEQ) && p.len == 2 && p.payload[1] == 1,
          "the GPL request parses too");
    CHECK(shp_sync_parse(&p, &s) == 0, "a request is not a response");

    /* garbage, XON / XOFF and the 0x80 burst between packets do not break the stream */
    uint8_t noisy[80];
    unsigned o = 0;
    for (int i = 0; i < 9; ++i)
        noisy[o++] = 0x80;
    noisy[o++] = 0x11;
    memcpy(noisy + o, gpl_rsp, 4);
    o += 4;
    noisy[o++] = 0x13;
    memcpy(noisy + o, gpl_rsp + 4, sizeof(gpl_rsp) - 4);
    o += sizeof(gpl_rsp) - 4;
    shp_rx_init(&r);
    CHECK(feed(&r, noisy, o, &p) == 1 && r.garbage == 9 && r.good == 1, "noise around a packet (%u garbage)", r.garbage);

    uint8_t bad[sizeof(gpl_rsp)];
    memcpy(bad, gpl_rsp, sizeof(bad));
    bad[8] ^= 1;
    shp_rx_init(&r);
    CHECK(feed(&r, bad, sizeof(bad), &p) == -1 && r.bad == 1, "one flipped bit is refused");

    /* sample1: a plain packet with one sensor RPC */
    shp_rx_init(&r);
    CHECK(feed(&r, gpl_sample1, sizeof(gpl_sample1), &p) == 1, "sample1 parses");
    CHECK(p.seq == 0x85 && p.flags == 0 && p.len == 0x26 && p.crc == 0xB2, "sample1's header (seq %02x len %u)", p.seq, p.len);
    unsigned off = 0;
    uint16_t id = 0, len = 0;
    const uint8_t *d = 0;
    CHECK(shp_rpc_next(p.payload, p.len, &off, &id, &d, &len) == 1 && id == SHP_RPC_SENSOR_RSP && len == 0x24,
          "sample1's RPC is 0x21 with 36 bytes (%x, %u)", id, len);
    CHECK(d[0] == 0x22 && d[1] == 0, "its SSP length is 34");
    static struct shp_ssp_rx sr;
    unsigned used = 0;
    shp_ssp_rx_init(&sr);
    int ev = shp_ssp_feed(&sr, d + 2, len - 2U, &used);
    CHECK(ev == SSP_EV_FRAME && used == 34 && sr.n == 30 && sr.buf[0] == 0x37, "its SSP bytes are one 30-byte data frame (%d, %u)", ev, sr.n);
    CHECK(shp_rpc_next(p.payload, p.len, &off, &id, &d, &len) == 0, "and nothing after it");

    /* a round trip with everything that needs care: stuffing, a long payload, flags with details */
    static uint8_t pl[1000], st[SHP_STUFFED_MAX];
    for (unsigned i = 0; i < sizeof(pl); ++i)
        pl[i] = (uint8_t)(i % 7 == 0 ? 0xB0 : i % 11 == 0 ? 0x11 : i % 13 == 0 ? 0x13 : i * 31);
    struct shp_pkt q = { 0 };
    q.seq = 0xB0;
    q.flags = SHP_F_ACK | SHP_F_RELIABLE | SHP_F_REL_ACK;
    q.ack = 0x11;
    q.rel = 0x13;
    q.rel_ack = 0xB0;
    q.payload = pl;
    q.len = sizeof(pl);
    n = shp_pkt_build(st, sizeof(st), &q);
    CHECK(n > sizeof(pl) + 8, "a long reliable packet builds (%u)", n);
    int raw = 0;
    for (unsigned i = 2; i + 2 < n; ++i)
        raw += st[i] == 0x11 || st[i] == 0x13;
    CHECK(raw == 0, "no raw XON / XOFF inside");
    shp_rx_init(&r);
    CHECK(feed(&r, st, n, &p) == 1, "and parses back");
    CHECK(p.seq == 0xB0 && p.ack == 0x11 && p.rel == 0x13 && p.rel_ack == 0xB0 && p.len == sizeof(pl) &&
          (p.flags & SHP_F_SIZE_EXT) && !memcmp(p.payload, pl, sizeof(pl)), "with the same fields and payload");
    q.len = SHP_MAX_PAYLOAD + 1;
    CHECK(shp_pkt_build(st, sizeof(st), &q) == 0, "a payload over the limit is refused");

    /* an ACK | RELIABLE_ACK answer as the receiver must send it */
    struct shp_pkt ack = { 0 };
    ack.seq = 7;
    ack.flags = SHP_F_ACK | SHP_F_REL_ACK;
    ack.ack = 0x42;
    ack.rel_ack = 3;
    n = shp_pkt_build(st, sizeof(st), &ack);
    static const uint8_t want[] = { 0xB0, 0x00, 0x07, 0x00, 0x05, 0x42, 0x03 };
    CHECK(n == sizeof(want) + 3 && !memcmp(st, want, sizeof(want)), "the acknowledgement's bytes");
}

static void t_rpc_ssi(void)
{
    uint8_t b[400], data[300];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)i;
    unsigned n = shp_rpc_put(b, sizeof(b), 0x00, 0, 0);
    CHECK(n == 2 && b[0] == 0 && b[1] == 0, "get version is 00 00");
    n += shp_rpc_put(b + n, sizeof(b) - n, 0x21, data, 300);
    CHECK(n == 2 + 3 + 300 && b[3] == 0x81 && b[4] == 0x2C, "a 300-byte RPC has a two-byte length (%02x %02x)", b[3], b[4]);
    unsigned off = 0;
    uint16_t id, len;
    const uint8_t *d;
    CHECK(shp_rpc_next(b, n, &off, &id, &d, &len) == 1 && id == 0 && len == 0, "first RPC");
    CHECK(shp_rpc_next(b, n, &off, &id, &d, &len) == 1 && id == 0x21 && len == 300 && !memcmp(d, data, 300), "second RPC");
    CHECK(shp_rpc_next(b, n, &off, &id, &d, &len) == 0, "the end");
    off = 0;
    CHECK(shp_rpc_next(b, n - 1, &off, &id, &d, &len) == 1 && shp_rpc_next(b, n - 1, &off, &id, &d, &len) == -1, "a cut RPC is malformed");

    uint8_t mosi[32], miso[32] = { 0x0B, 0x02, 0x00, 0xAA, 0xBB, 0, 0, 0, 0x05, 0x00 };
    n = shp_ssi_wframe(mosi, sizeof(mosi), data + 1, 5);
    CHECK(n == 10 && mosi[0] == 0x58 && mosi[1] == 5 && mosi[2] == 0 && mosi[3] == 1 && mosi[7] == 5 && mosi[8] == 0 && mosi[9] == 0, "a write frame");
    CHECK(shp_ssi_wframe(mosi, 9, data, 5) == 0, "a write frame that does not fit");
    struct shp_ssi_w w;
    shp_ssi_wparse(miso, 5, &w);
    CHECK(w.status == 0x0B && w.n == 2 && w.chip[0] == 0xAA && w.accepted == 5, "its answer (%u %u)", w.n, w.accepted);
    miso[1] = 9;
    shp_ssi_wparse(miso, 5, &w);
    CHECK(w.n == 5, "chip bytes are cut to what the frame held");
    CHECK(shp_ssi_space(0x0B) == 3 * 256 && shp_ssi_space(0x1F) == 7 * 16384 && shp_ssi_space(0x40) == 0, "the status byte's free space");
}

static void t_rom(const char *patch)
{
    static const uint8_t le[12] = { 0xB0, 0x75, 0x47, 0x00, 0x7C, 0x52, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t be[12] = { 0x00, 0x47, 0x75, 0xB0, 0x00, 0x04, 0x52, 0x7C, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t no[12] = { 0x00, 0x00, 0x73, 0x47 };
    struct shp_version v;
    CHECK(shp_version_parse(le, 12, &v) == 1 && v.asic == 0x004775B0 && v.rom == 0x0004527C && !v.big_endian, "version, little-endian");
    CHECK(shp_version_parse(be, 12, &v) == 1 && v.asic == 0x004775B0 && v.rom == 0x0004527C && v.patch == 1 && v.big_endian, "version, big-endian");
    CHECK(shp_version_parse(no, 12, &v) == 0 && shp_version_parse(le, 11, &v) == 0, "another chip or a short answer is not recognised");

    uint8_t blob[64] = { 0xF1, 0x08, 0x00, 0x00, 0x00, 0x80, 0x00, 1, 2, 3, 4,
                         0xF1, 0x06, 0x00, 0x04, 0x00, 0x80, 0x00, 5, 6 };
    unsigned off = 0, len = 0;
    uint32_t addr = 0;
    const uint8_t *d = 0;
    CHECK(shp_patch_next(blob, 20, &off, &addr, &d, &len) == 1 && addr == 0x00800000 && len == 4 && d[3] == 4, "patch record 1");
    CHECK(shp_patch_next(blob, 20, &off, &addr, &d, &len) == 1 && addr == 0x00800004 && len == 2 && d[1] == 6, "patch record 2");
    CHECK(shp_patch_next(blob, 20, &off, &addr, &d, &len) == 0, "patch end");
    off = 0;
    CHECK(shp_patch_next(blob, 19, &off, &addr, &d, &len) == 1 && shp_patch_next(blob, 19, &off, &addr, &d, &len) == -1, "a cut record is malformed");
    blob[0] = 0xF0;
    off = 0;
    CHECK(shp_patch_next(blob, 20, &off, &addr, &d, &len) == -1, "another record type is refused");

    uint8_t r[32];
    unsigned n = shp_rpc_write_mem(r, sizeof(r), 0x00800004, blob + 18, 2, 0);
    static const uint8_t wl[] = { 0x05, 0x08, 0x04, 0x00, 0x80, 0x00, 0x02, 0x00, 5, 6 };
    static const uint8_t wb[] = { 0x05, 0x08, 0x00, 0x80, 0x00, 0x04, 0x00, 0x02, 5, 6 };
    CHECK(n == sizeof(wl) && !memcmp(r, wl, n), "RPC 0x05, little-endian");
    n = shp_rpc_write_mem(r, sizeof(r), 0x00800004, blob + 18, 2, 1);
    CHECK(n == sizeof(wb) && !memcmp(r, wb, n), "RPC 0x05, big-endian");
    n = shp_rpc_start(r, sizeof(r), 0x00800161, 0);
    static const uint8_t sl[] = { 0x2E, 0x04, 0x61, 0x01, 0x80, 0x00 };
    CHECK(n == sizeof(sl) && !memcmp(r, sl, n), "RPC 0x2E");

    if (!patch)
        return;
    FILE *f = fopen(patch, "rb");
    CHECK(f != 0, "the patch blob opens");
    if (!f)
        return;
    static uint8_t pb[500000], rp[1100], st[SHP_STUFFED_MAX];
    unsigned pn = (unsigned)fread(pb, 1, sizeof(pb), f);
    fclose(f);
    unsigned recs = 0, bytes = 0, maxl = 0, maxst = 0;
    uint32_t next = 0x00800000;
    int contiguous = 1, rv;
    off = 0;
    while ((rv = shp_patch_next(pb, pn, &off, &addr, &d, &len)) == 1) {
        ++recs;
        bytes += len;
        contiguous &= addr == next;
        next = addr + len;
        if (len > maxl)
            maxl = len;
        n = shp_rpc_write_mem(rp, sizeof(rp), addr, d, len, 0);
        struct shp_pkt q = { 0 };
        q.flags = SHP_F_RELIABLE;
        q.payload = rp;
        q.len = (uint16_t)n;
        unsigned sn = shp_pkt_build(st, sizeof(st), &q);
        if (!n || !sn)
            contiguous = 0;
        if (sn > maxst)
            maxst = sn;
    }
    CHECK(pn == 413864 && rv == 0 && recs == 452 && bytes == 410700 && maxl <= 916 && contiguous && next == 0x0086444C,
          "the patch: %u bytes, %u records, %u data bytes, longest %u, to %08x", pn, recs, bytes, maxl, next);
    CHECK(maxst + SHP_SSI_OVERHEAD <= 4096, "the longest stuffed write fits one 4 KiB frame (%u)", maxst);
    CHECK(pb[11] == 0x61 && pb[12] == 0x01 && pb[13] == 0x80 && pb[14] == 0x00, "its reset vector is the PatchAddress");
    printf("shub host test: the patch blob: %u records, %u bytes, longest stuffed packet %u\n", recs, bytes, maxst);
}

static void t_ssp(void)
{
    uint8_t h[SSP_HDR], e[10], w[64];
    shp_ssp_hdr(h, SSP_CMD_WHOAMI, 1, SSP_READ, 0);
    static const uint8_t who[] = { 0x0F, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0 };
    CHECK(!memcmp(h, who, 9), "the who-am-I request");
    shp_ssp_enable(e, SSP_SENSOR_MAG, 100, 0, 0);
    static const uint8_t en[] = { 0x04, 100, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!memcmp(e, en, 10), "the enable body");
    unsigned n = shp_ssp_wrap(w, sizeof(w), h, SSP_HDR);
    CHECK(n == 13 && w[0] == 0x20 && w[1] == 11 && w[2] == 9 && w[3] == 0 && w[4] == 0x0F, "the request as RPC 0x20");

    static struct shp_ssp_rx s;
    unsigned used = 0;
    shp_ssp_rx_init(&s);
    /* a read answer, its header and body in separate pieces */
    static const uint8_t rh[] = { 0x00, 0x00, 0x01, 0x00 }, rb[] = { 0x55 };
    CHECK(shp_ssp_feed(&s, rh, 4, &used) == SSP_EV_ERR, "a read answer nobody asked for resets the stream");
    s.read_len = 1;
    CHECK(shp_ssp_feed(&s, rh, 2, &used) == SSP_EV_NONE && used == 2, "half a header");
    CHECK(shp_ssp_feed(&s, rh + 2, 2, &used) == SSP_EV_NONE && used == 2, "the other half");
    CHECK(shp_ssp_feed(&s, rb, 1, &used) == SSP_EV_READ && s.n == 1 && s.buf[0] == 0x55 && s.read_len == 0, "the read's result");
    /* a write echo, then a data frame right behind it in the same bytes */
    static const uint8_t mix[] = { 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x0A, 0x02, 0x00 };
    CHECK(shp_ssp_feed(&s, mix, sizeof(mix), &used) == SSP_EV_WRITE_GO && used == 4 && s.opts == 1, "a write's echo");
    unsigned u2 = 0;
    CHECK(shp_ssp_feed(&s, mix + used, sizeof(mix) - used, &u2) == SSP_EV_FRAME && u2 == 5 && s.n == 1 && s.buf[0] == 0x0A,
          "the init-done frame behind it");
    CHECK(shp_ssp_feed(&s, mix + 9, 2, &u2) == SSP_EV_NONE && u2 == 2, "the start of the next header waits");
    static const uint8_t junk[] = { 0x03, 0x00, 0x00, 0x00 };
    shp_ssp_rx_init(&s);
    CHECK(shp_ssp_feed(&s, junk, 4, &used) == SSP_EV_ERR, "an unknown type resets the stream");

    /* a data frame: timestamp offset, accelerometer, magnetometer, step counter, meta, then an unknown type */
    static const uint8_t fr[] = {
        0xA8, 1, 2, 3, 4, 5, 6, 7, 8,
        0x37, 0x00, 0x1F, 0x00, 0xC7, 0xFF, 0xEC, 0x0F, 0x10, 0, 0, 0, 0, 0, 0, 0,
        0x37, 0x04, 0x9C, 0xFF, 0x2C, 0x01, 0x38, 0xFF, 0x03, 0x00, 0x20, 0, 0, 0, 0, 0, 0, 0,
        0x37, 0x12, 0x05, 0x00, 0x00, 0x00, 0x30, 0, 0, 0, 0, 0, 0, 0,
        0x05, 0x01, 0x00,
        0x0A,
        0x99, 1, 2 };
    struct shp_rec r;
    unsigned off = 0;
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.kind == SSP_REC_OTHER && r.type == 0xA8, "record: timestamp offset");
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.kind == SSP_REC_SENSOR && r.sensor == 0 && r.x == 31 && r.y == -57 &&
          r.z == 4076 && r.ts == 0x10, "record: accelerometer (%d %d %d)", r.x, r.y, r.z);
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.sensor == 4 && r.x == -100 && r.y == 300 && r.z == -200 &&
          r.accuracy == 3 && r.overflow == 0 && r.ts == 0x20, "record: magnetometer");
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.sensor == 18 && r.steps == 5 && r.ts == 0x30, "record: step counter");
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.type == 0x05, "record: meta");
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == 1 && r.kind == SSP_REC_INIT_DONE, "record: init done");
    CHECK(shp_frame_next(fr, sizeof(fr), &off, &r) == -1, "an unknown type drops the rest");
    off = 9;
    CHECK(shp_frame_next(fr, 9 + 15, &off, &r) == -1, "a cut sample is refused");
    static const uint8_t unk[] = { 0x37, 0x63, 0, 0 };
    off = 0;
    CHECK(shp_frame_next(unk, sizeof(unk), &off, &r) == -1, "a sensor of unknown length drops the rest");
}

static void t_compass(void)
{
    int d = -1, p = 0, r = 0;
    /* flat, the field 40 uT north and 30 uT down: the top edge to the north, east, south, west */
    CHECK(shp_heading(0, 0, 4096, 0, 400, -300, &d) == 1 && d == 0, "flat, top edge north (%d)", d);
    CHECK(shp_heading(0, 0, 4096, -400, 0, -300, &d) == 1 && d == 900, "flat, top edge east (%d)", d);
    CHECK(shp_heading(0, 0, 4096, 0, -400, -300, &d) == 1 && d == 1800, "flat, top edge south (%d)", d);
    CHECK(shp_heading(0, 0, 4096, 400, 0, -300, &d) == 1 && d == 2700, "flat, top edge west (%d)", d);
    /* every heading and many tilts against libm: the phone turned by yaw, then pitched and rolled */
    int worst = 0;
    for (int yaw = 0; yaw < 360; yaw += 5)
        for (int pit = -60; pit <= 60; pit += 30)
            for (int rol = -60; rol <= 60; rol += 30) {
                double y = yaw * M_PI / 180, pt = pit * M_PI / 180, rl = rol * M_PI / 180;
                /* world: x east, y north, z up; the phone's axes after yaw (clockwise from north), pitch, roll */
                double xa[3] = { cos(y), -sin(y), 0 }, ya[3] = { sin(y), cos(y), 0 }, za[3] = { 0, 0, 1 };
                double y2[3], z2[3], x3[3], z3[3];
                for (int i = 0; i < 3; ++i) {                   /* pitch about x: the top edge up */
                    y2[i] = cos(pt) * ya[i] + sin(pt) * za[i];
                    z2[i] = -sin(pt) * ya[i] + cos(pt) * za[i];
                }
                for (int i = 0; i < 3; ++i) {                   /* roll about the new y: the right edge up */
                    x3[i] = cos(rl) * xa[i] + sin(rl) * z2[i];
                    z3[i] = -sin(rl) * xa[i] + cos(rl) * z2[i];
                }
                double g[3] = { 0, 0, 4096 }, m[3] = { 0, 400, -300 };
                int a[3], e[3];
                a[0] = (int)lround(g[0] * x3[0] + g[1] * x3[1] + g[2] * x3[2]);
                a[1] = (int)lround(g[0] * y2[0] + g[1] * y2[1] + g[2] * y2[2]);
                a[2] = (int)lround(g[0] * z3[0] + g[1] * z3[1] + g[2] * z3[2]);
                e[0] = (int)lround(m[0] * x3[0] + m[1] * x3[1] + m[2] * x3[2]);
                e[1] = (int)lround(m[0] * y2[0] + m[1] * y2[1] + m[2] * y2[2]);
                e[2] = (int)lround(m[0] * z3[0] + m[1] * z3[1] + m[2] * z3[2]);
                if (!shp_heading(a[0], a[1], a[2], e[0], e[1], e[2], &d)) {
                    worst = 9999;
                    continue;
                }
                /* the reference: the top edge's horizontal direction (pitch below 90: it keeps the yaw) */
                double want = atan2(y2[0], y2[1]) * 180 / M_PI;
                int diff = (int)lround(fabs(fmod(d / 10.0 - want + 540.0 + 360.0, 360.0) - 180.0) * 10);
                if (diff > worst)
                    worst = diff;
            }
    CHECK(worst <= 5, "tilt-compensated heading within 0.5 degrees of the reference at every yaw, pitch and roll (%d)", worst);
    CHECK(shp_heading(0, 0, 0, 0, 400, -300, &d) == 0 && shp_heading(0, 0, 4096, 0, 0, 0, &d) == 0, "no gravity or no field: no heading");
    CHECK(shp_heading(0, 0, 4096, 0, 0, -500, &d) == 0, "the field straight down: no heading");

    CHECK(shp_tilt(0, 0, 4096, &p, &r) == 1 && p == 0 && r == 0, "flat: level");
    CHECK(shp_tilt(0, 4096, 0, &p, &r) == 1 && p == 900 && r == 0, "top edge up: pitch 90 (%d %d)", p, r);
    CHECK(shp_tilt(4096, 0, 0, &p, &r) == 1 && p == 0 && r == 900, "right edge up: roll 90");
    CHECK(shp_tilt(0, -2048, 3547, &p, &r) == 1 && p == -300 && r == 0, "top edge 30 down (%d)", p);
    CHECK(shp_tilt(2896, 0, 2896, &p, &r) == 1 && r == 450, "right edge 45 up (%d)", r);
    CHECK(shp_tilt(0, 0, 0, &p, &r) == 0, "no sample: no tilt");
}

int main(int argc, char **argv)
{
    t_packets();
    t_rpc_ssi();
    t_rom(argc > 1 ? argv[1] : 0);
    t_ssp();
    t_compass();
    printf("shub host test: %d checks, %d failed%s\n", checks, fails, argc > 1 ? "" : " (no patch blob given: its records not checked)");
    return fails ? 1 : 0;
}
