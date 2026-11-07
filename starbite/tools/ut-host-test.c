/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the Utility OS's USB mass storage (msc.c) and FAT32 formatter (fat.c), on Linux:
 *   gcc -O1 -Wall -Wextra -Istarbite/utility -o ut-test starbite/tools/ut-host-test.c starbite/utility/msc.c starbite/utility/fat.c && ./ut-test [fat.img]
 * (starbite/utility/build.sh runs it on a native aarch64 host before building the image)
 * The DMA window is mapped at its real address (0xa7000000); a fake controller records the queued
 * transfers and the test plays the host. fat.img: the formatted 512 MiB image, for fsck.fat -n.
 * Also: the configuration descriptor (usb_desc.c: total length, the descriptor chain, the HID report
 * descriptor's length and collections). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "msc.h"
#include "fat.h"
#include "usb_desc.h"
#include "hash.h"
#include "calc.h"

static int busy[16];
static uintptr_t qbuf[16];
static uint32_t qlen[16];
int usbd_queue(unsigned phys, uintptr_t buf, uint32_t len) { if (busy[phys]) return -1; busy[phys] = 1; qbuf[phys] = buf; qlen[phys] = len; return 0; }
int usbd_busy(unsigned phys) { return busy[phys]; }
int usbd_configured(void) { return 1; }
int usbd_full_speed(void) { return 0; }

#define PIN USB_PHYS(USB_EP_MSC_IN)
#define POUT USB_PHYS(USB_EP_MSC_OUT)
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static uint8_t *ram;
static int rd(struct msc_lun *l, uint64_t lba, uint32_t n, uint8_t *d) { (void)l; memcpy(d, ram + lba * 512, n * 512); return 0; }
static int wr(struct msc_lun *l, uint64_t lba, uint32_t n, const uint8_t *s) { (void)l; memcpy(ram + lba * 512, s, n * 512); return 0; }
static int nowr(struct msc_lun *l, uint64_t lba, uint32_t n, const uint8_t *s) { (void)l; (void)lba; (void)n; (void)s; return -1; }

static void done(unsigned phys, uint32_t n)
{
    struct usbd_ev e = { USBD_EV_DONE, phys, n, 0 };
    busy[phys] = 0;
    msc_event(&e);
    msc_service();
}

/* One command: CBW, data (in: collected into out; out: from data), CSW. Returns the CSW status, *res the residue. */
static int cmd(unsigned lun, int in, uint32_t h, const uint8_t *cdb, unsigned clen, uint8_t *out, const uint8_t *data, uint32_t *res)
{
    static uint32_t tag = 100;
    msc_service();
    CHECK(busy[POUT] && qlen[POUT] >= 31, "no CBW queued");
    uint8_t *c = (uint8_t *)qbuf[POUT];
    memset(c, 0, 31);
    memcpy(c, "USBC", 4);
    ++tag;
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &h, 4);
    c[12] = in ? 0x80 : 0;
    c[13] = (uint8_t)lun;
    c[14] = (uint8_t)clen;
    memcpy(c + 15, cdb, clen);
    done(POUT, 31);
    uint32_t moved = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        if (busy[PIN] && qlen[PIN] == 13 && memcmp((void *)qbuf[PIN], "USBS", 4) == 0 && (moved >= h || !in)) {
            uint8_t *s = (uint8_t *)qbuf[PIN];
            uint32_t t;
            memcpy(&t, s + 4, 4);
            CHECK(t == tag, "CSW tag %u != %u", t, tag);
            memcpy(res, s + 8, 4);
            int st = s[12];
            done(PIN, 13);
            return st;
        }
        if (in && busy[PIN]) {
            uint32_t n = qlen[PIN];
            if (out)
                memcpy(out + moved, (void *)qbuf[PIN], n);
            moved += n;
            done(PIN, n);
        } else if (!in && busy[POUT]) {
            uint32_t n = h - moved < qlen[POUT] ? h - moved : qlen[POUT];
            memcpy((void *)qbuf[POUT], data + moved, n);
            moved += n;
            done(POUT, n);
        } else {
            CHECK(0, "stuck: no transfer queued (moved %u of %u)", moved, h);
            return -1;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    void *dma = mmap((void *)USB_DMA_BASE, USB_DMA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (dma != (void *)USB_DMA_BASE) {
        printf("cannot map the DMA window at %#lx\n", (unsigned long)USB_DMA_BASE);
        return 2;
    }
    const uint64_t bytes = 512ULL << 20;
    ram = malloc(bytes);
    memset(ram, 0xa5, bytes);
    struct fat32_info fi;
    CHECK(fat32_format(ram, bytes / 512, "STARBITE", 0x12345678, &fi) == 0, "format");
    CHECK(ram[510] == 0x55 && ram[511] == 0xaa && memcmp(ram + 82, "FAT32", 5) == 0, "boot sector");
    CHECK(fi.clusters >= 65525, "clusters %u", fi.clusters);
    CHECK(fat32_format(ram, (128ULL << 20) / 512, "X", 1, 0) == -1, "128 MiB must be refused");
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fwrite(ram, 1, bytes, f);
        fclose(f);
    }

    struct msc_lun l[2];
    memset(l, 0, sizeof(l));
    strcpy(l[0].vendor, "Starbite"); strcpy(l[0].product, "RAM disk"); strcpy(l[0].serial, "SBRAM001");
    l[0].blocks = bytes / 512; l[0].present = 1; l[0].read = rd; l[0].write = wr;
    strcpy(l[1].vendor, "Starbite"); strcpy(l[1].product, "microSD (ro)"); strcpy(l[1].serial, "SBSD0001");
    l[1].blocks = 1000; l[1].ro = 1; l[1].present = 1; l[1].read = rd; l[1].write = nowr;
    msc_setup(l, 2);
    CHECK(usb_msc_max_lun == 1, "max lun");

    uint8_t buf[1 << 20], data[1 << 20];
    uint32_t res;
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    CHECK(cmd(0, 1, 36, inq, 6, buf, 0, &res) == 0 && res == 0, "INQUIRY status/res %u", res);
    CHECK(memcmp(buf + 8, "Starbite", 8) == 0 && memcmp(buf + 16, "RAM disk", 8) == 0 && buf[1] == 0x80, "INQUIRY data");
    uint8_t cap[10] = { 0x25 };
    CHECK(cmd(0, 1, 8, cap, 10, buf, 0, &res) == 0, "READ CAPACITY");
    uint32_t last = (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
    CHECK(last == bytes / 512 - 1 && buf[6] == 2 && buf[7] == 0, "capacity %u", last);
    uint8_t tur[6] = { 0 };
    CHECK(cmd(0, 0, 0, tur, 6, 0, 0, &res) == 0, "TEST UNIT READY");
    /* write 300 KiB (crosses the 128 KiB chunk twice) at LBA 4096, read it back */
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(i * 7 + 3);
    uint32_t nb = 600;
    uint8_t w10[10] = { 0x2a, 0, 0, 0, 0x10, 0x00, 0, (uint8_t)(nb >> 8), (uint8_t)nb, 0 };
    CHECK(cmd(0, 0, nb * 512, w10, 10, 0, data, &res) == 0 && res == 0, "WRITE(10) res %u", res);
    CHECK(memcmp(ram + 4096 * 512, data, nb * 512) == 0, "written data in RAM");
    CHECK(l[0].wr_bytes == nb * 512, "wr_bytes %llu", (unsigned long long)l[0].wr_bytes);
    uint8_t r10[10] = { 0x28, 0, 0, 0, 0x10, 0x00, 0, (uint8_t)(nb >> 8), (uint8_t)nb, 0 };
    memset(buf, 0, sizeof(buf));
    CHECK(cmd(0, 1, nb * 512, r10, 10, buf, 0, &res) == 0 && res == 0, "READ(10)");
    CHECK(memcmp(buf, data, nb * 512) == 0, "read back == written");
    /* host asks 16 bytes more than the read has: padded, residue 16, good */
    uint8_t r1[10] = { 0x28, 0, 0, 0, 0x10, 0x00, 0, 0, 1, 0 };
    CHECK(cmd(0, 1, 528, r1, 10, buf, 0, &res) == 0 && res == 16, "READ host-more res %u", res);
    /* unknown opcode: failed, then REQUEST SENSE says ILLEGAL REQUEST / invalid opcode */
    uint8_t bad[6] = { 0xc7 };
    CHECK(cmd(0, 1, 0, bad, 6, 0, 0, &res) == 1, "unknown opcode must fail");
    uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
    CHECK(cmd(0, 1, 18, rs, 6, buf, 0, &res) == 0 && buf[2] == 0x05 && buf[12] == 0x20, "REQUEST SENSE %02x %02x", buf[2], buf[12]);
    /* the read-only card: a write is refused (data received and dropped), DATA PROTECT */
    uint8_t w1[10] = { 0x2a, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
    CHECK(cmd(1, 0, 1024, w1, 10, 0, data, &res) == 1, "write to read-only must fail");
    CHECK(cmd(1, 1, 18, rs, 6, buf, 0, &res) == 0 && buf[2] == 0x07 && buf[12] == 0x27, "DATA PROTECT sense");
    uint8_t ms[6] = { 0x1a, 0, 0x3f, 0, 192, 0 };
    CHECK(cmd(1, 1, 192, ms, 6, buf, 0, &res) == 0 && (buf[2] & 0x80) && res == 188, "MODE SENSE(6) WP, res %u", res);
    /* out of range */
    uint8_t oor[10] = { 0x28, 0, 0, 0, 0x03, 0xe8, 0, 0, 1, 0 };
    CHECK(cmd(1, 1, 512, oor, 10, buf, 0, &res) == 1, "LBA 1000 of 1000 must fail");
    /* a bulk-only reset mid-way leaves the transport ready for the next CBW */
    usb_msc_reset = 1;
    msc_service();
    CHECK(cmd(0, 1, 36, inq, 6, buf, 0, &res) == 0, "INQUIRY after reset");
    /* SHA-256 (FIPS 180-2 examples) and CRC32 ("123456789" -> cbf43926) */
    {
        struct sha256 h;
        uint8_t d[32];
        static const uint8_t abc[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
                                         0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                         0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
        static const uint8_t mil[32] = { 0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7, 0xe2,
                                         0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
                                         0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0 };
        sha256_init(&h);
        sha256_update(&h, (const uint8_t *)"abc", 3);
        sha256_final(&h, d);
        CHECK(memcmp(d, abc, 32) == 0, "sha256(abc)");
        static uint8_t a[1000000];
        memset(a, 'a', sizeof(a));
        sha256_init(&h);
        sha256_update(&h, a, 333333);                 /* in uneven pieces */
        sha256_update(&h, a + 333333, 1);
        sha256_update(&h, a + 333334, sizeof(a) - 333334);
        sha256_final(&h, d);
        CHECK(memcmp(d, mil, 32) == 0, "sha256(1M x a)");
        CHECK(crc32_update(0, (const uint8_t *)"123456789", 9) == 0xcbf43926U, "crc32");
        CHECK(crc32_update(crc32_update(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xcbf43926U,
              "crc32 in two parts");
    }
    /* the calculator's math and parser */
    {
        double v;
        struct { const char *e; double want; } t[] = {
            { "2+3*4", 14 }, { "(2+3)*4", 20 }, { "2^10", 1024 }, { "2^0.5", 1.41421356237309505 },
            { "sqrt(2)", 1.41421356237309505 }, { "sin(30)", 0.5 }, { "cos(60)", 0.5 }, { "tan(45)", 1 },
            { "ln(e)", 1 }, { "log(1000)", 3 }, { "-3+10/4", -0.5 }, { "pi*2", 6.28318530717958648 },
            { "sin(390)", 0.5 }, { "1/3*3", 1 },
        };
        for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
            int r = calc_eval(t[i].e, &v);
            double d = v - t[i].want;
            CHECK(r == 0 && d < 1e-10 && d > -1e-10, "calc %s = %.15g (want %.15g)", t[i].e, v, t[i].want);
        }
        CHECK(calc_eval("2+", &v) != 0 && calc_eval("(1", &v) != 0, "malformed expressions refused");
        double ex = cm_exp(1) - 2.718281828459045, lx = cm_ln(1e-5) + 11.512925464970229;
        CHECK(ex < 1e-13 && ex > -1e-13 && lx < 1e-11 && lx > -1e-11, "exp / ln");
        char o[64];
        calc_format(1.0 / 3, o, sizeof(o));
        CHECK(strcmp(o, "0.333333333333") == 0, "format 1/3: %s", o);
        calc_format(1234567.5, o, sizeof(o));
        CHECK(strcmp(o, "1234567.5") == 0, "format 1234567.5: %s", o);
        calc_format(-2.5e20, o, sizeof(o));
        CHECK(strcmp(o, "-2.5e20") == 0, "format -2.5e20: %s", o);
        calc_format(0.000001234, o, sizeof(o));
        CHECK(strcmp(o, "0.000001234") == 0, "format 0.000001234: %s", o);
        calc_format(0.0000001234, o, sizeof(o));
        CHECK(strcmp(o, "1.234e-7") == 0, "format 1.234e-7: %s", o);
    }
    /* the configuration descriptor, both speeds */
    for (int fs = 0; fs < 2; ++fs) {
        uint8_t cfg[256];
        unsigned n = usb_desc_config(cfg, fs, 2);
        CHECK(n == USB_CONFIG_LEN && (unsigned)(cfg[2] | cfg[3] << 8) == n, "config length %u (wTotalLength %u, want %u)",
              n, cfg[2] | cfg[3] << 8, USB_CONFIG_LEN);
        CHECK(cfg[4] == USB_IF_COUNT, "bNumInterfaces %u", cfg[4]);
        unsigned off = 0, ifs = 0, eps = 0, hid_len = 0;
        while (off < n) {
            CHECK(cfg[off] >= 2 && off + cfg[off] <= n, "descriptor chain broken at %u", off);
            if (cfg[off] < 2)
                break;
            if (cfg[off + 1] == 4)
                ++ifs;
            if (cfg[off + 1] == 5)
                ++eps;
            if (cfg[off + 1] == 0x21)
                hid_len = (unsigned)(cfg[off + 7] | cfg[off + 8] << 8);
            off += cfg[off];
        }
        CHECK(off == n && ifs == 4 && eps == 6, "chain: end %u, %u interfaces, %u endpoints", off, ifs, eps);
        uint8_t rep[256];
        unsigned rn = usb_hid_report_desc(rep);
        CHECK(hid_len == rn, "HID descriptor says %u, report descriptor is %u", hid_len, rn);
        int depth = 0;
        for (unsigned i = 0; i < rn;) {               /* short items: size bits 0..1 = 0,1,2,4 bytes */
            uint8_t b = rep[i], sz = (uint8_t)(b & 3U);
            if ((b & 0xfcU) == 0xa0U)
                ++depth;
            if (b == 0xc0)
                --depth;
            i += 1U + (sz == 3 ? 4U : sz);
        }
        CHECK(depth == 0, "report descriptor collections unbalanced (%d)", depth);
    }
    printf("%s: %llu commands, %d failed checks\n", fails ? "FAILED" : "PASSED", (unsigned long long)msc_stats.cmds, fails);
    return fails != 0;
}
